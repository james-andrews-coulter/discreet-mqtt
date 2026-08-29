/*
 * Discreet_MQTT.ino
 * ------------------------------------------------------------------
 * MQTT conversion of the Discreet espresso controller (Gaggia Classic mod).
 * Based on: https://github.com/Discreet-Coffee/Discreet (GPL v3)
 *
 * What changed vs stock Discreet:
 *   - REMOVED the entire HTTP web server, SD-card web UI, themes,
 *     file upload/delete, and config-served front end.
 *   - SD card is now used ONCE at boot to read config.json (WiFi +
 *     MQTT credentials + PID tuning), then shut down for good.
 *   - Added an MQTT client (PubSubClient) for Home Assistant:
 *       publishes  : discreet/telemetry  (JSON, ~1 Hz)
 *                     discreet/status     (online/offline, LWT)
 *       subscribes : discreet/cmd/#  (setpoint, pressuresetpoint,
 *                     preinftime, bloomtime, steam, pause)
 *   - Shot logic, PID, pressure loop, dimmer, steam detection,
 *     buzzer and OTA are byte-for-byte the stock behaviour.
 *
 * Libraries (Arduino Library Manager):
 *   - PubSubClient by Nick O'Leary   (NEW)
 *   - everything else identical to stock Discreet
 *
 * config.json on the SD card now also accepts:
 *   "mqtt_host": "192.168.1.50",
 *   "mqtt_port": 1883,
 *   "mqtt_user": "mqttuser",
 *   "mqtt_pass": "mqttpass"
 *   (user/pass optional; host required for MQTT to start)
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <SD.h>
#include <SPI.h>
#include <dimmable_light.h>
#include <Wire.h>
#include "max6675.h"
#include <PID_v1.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>

// ---------------- WiFi / MQTT config (loaded from SD at boot) ----------------
String ssid;
String password;
String mqttHost;
int mqttPort = 1883;
String mqttUser;
String mqttPass;
String mqttTopic = "discreet";          // base topic; all others hang off it

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---------------- Brew-by-weight: BLE scale (MY_SCALE / Blackcoffee FFB0) ----------------
// The scale is a "MY_SCALE" BLE peripheral sitting on the drip tray; this
// ESP32 is the BLE *central*.
//
// VERIFIED against James's actual scale by live GATT enumeration on
// 2026-08-28 (see docs/my-scale-ble-protocol.md):
//   name "MY_SCALE", service FFB0, notify char FFB2, write char FFB1,
//   20-byte packets @ ~6.7 Hz, header AC 40, weight = 28-bit big-endian
//   MILLIGRAMS spanning (b3 & 0x0F), b4, b5, b6.
// Decode cross-checked against two independent open-source implementations:
//   - GaggiMate  esp-arduino-ble-scales/src/scales/myscale.cpp  (MIT)
//   - Bean Conqueror src/classes/devices/blackcoffeeScale.ts    (GPLv3)
// Both agree exactly; host-side unit tests in test/test_myscale_parse.c.
#define SCALE_SERVICE_UUID   "0000FFB0-0000-1000-8000-00805F9B34FB"
#define SCALE_WEIGHT_UUID    "0000FFB2-0000-1000-8000-00805F9B34FB"
#define SCALE_COMMAND_UUID   "0000FFB1-0000-1000-8000-00805F9B34FB"

// Packet framing
#define SCALE_PKT_MINLEN     15      // GaggiMate rejects <15; BC requires >14
#define SCALE_HDR0           0xAC
#define SCALE_HDR1           0x40

// Concurrency note: these are written in the NimBLE host task and read in
// loop() (different FreeRTOS tasks, typically different cores). `volatile` is
// deliberate and sufficient HERE because each value is consumed
// INDEPENDENTLY - there is no "if (flag) then trust (value)" pair whose
// invariant could be broken by reordering:
//   - scaleRawWeight is a single aligned 32-bit float, so an ESP32 store is
//     one instruction and cannot tear. The cut logic reads only this value; a
//     one-packet-stale read (150 ms) is indistinguishable from normal notify
//     latency and is already absorbed by the predictive lead.
//   - scaleStable / scaleConnected are independent booleans (display + link
//     state), never used to gate the validity of the weight.
//   - scaleLastPacketMs is monotonic; a stale read only delays the watchdog
//     by one cycle.
// If you ever add logic of the form "only cut when scaleStable is true", that
// introduces a real invariant across two variables and you MUST switch to a
// portMUX_TYPE spinlock (or publish a struct atomically) instead.
volatile float scaleRawWeight = 0.0f; // latest ABSOLUTE grams from the scale
volatile bool scaleStable = false;    // scale reports the reading as settled
volatile bool scaleConnected = false; // BLE link up
volatile uint32_t scaleLastPacketMs = 0; // millis() of last good packet
bool scaleWasEverConnected = false;   // beep only on the FIRST connect

// How long the scale may go silent before we drop the link. This scale idles
// and deep-sleeps routinely, so a short timeout causes an endless
// scan->connect->sleep->drop cycle. A frozen reading cannot cause a premature
// cut (the cut requires weight to RISE to target), so a generous value is safe.
#define SCALE_SILENCE_MS 15000

float scaleTareOffset = 0.0f;         // software tare captured at shot start
uint32_t scaleTareSettleAt = 0;       // 0 = no pending tare; else deadline (ms)
#define SCALE_TARE_SETTLE_MS 400      // ~3 notify periods: enough for a HW tare
                                      // to land, short enough to precede any pour
bool scaleArmed = false;              // BBW armed for the current shot
bool bbwEnabled = true;               // master switch (config.json + MQTT)
float targetWeight = 36.0f;           // cut-off grams (config.json + MQTT)
bool cutOnScaleLoss = false;          // config: hard-cut if scale dies mid-shot
bool pumpCutByWeight = false;         // forces pump output to 0
bool shotCutByWeight = false;         // this shot already auto-cut
float shotWeight = 0.0f;              // NET weight at the moment of the cut

// --- Predictive cut tuning ---
// The pump keeps delivering, and the puck keeps dripping, for a short time
// after we cut. BBW_LEAD_TIME_S is how far ahead we predict: at 2 g/s a
// 0.35 s lead stops ~0.7 g early so the final settled weight lands on target.
// Raise it if shots consistently overshoot, lower it if they come up short.
#define BBW_LEAD_TIME_S   0.35f
#define BBW_MAX_LEAD_G    3.0f        // safety clamp on the predicted lead
float bbwFlowRate = 0.0f;             // measured extraction flow, g/s
uint32_t bbwLastFlowMs = 0;           // last flow-estimate timestamp
float bbwLastFlowWeight = 0.0f;       // net weight at last flow estimate

// Net (tared) weight the user actually cares about.
float scaleNetWeight() {
  return scaleRawWeight - scaleTareOffset;
}

// Forward declarations (definitions live in the BLE section further down).
void scaleTareNow();
void scaleSendTare();
void scaleServiceTare();

NimBLEAddress scaleFoundAddress((uint8_t*)"\x00\x00\x00\x00\x00\x00");
bool scaleFound = false;
static NimBLEClient* scaleClient = nullptr;
static NimBLERemoteCharacteristic* scaleWeightChr = nullptr;
static NimBLERemoteCharacteristic* scaleCmdChr = nullptr;

// SD card pins
#define SD_CS     32
#define SD_MOSI  25
#define SD_MISO  26
#define SD_SCK   33

// SSR Pins
#define SSR_PIN 13

// Dimmer Pins
#define syncPin        19   // Zero-cross sync input
#define thyristorPin   18   // Thyristor (dimmer PWM output)

// Thermocouple SPI (HSPI)
#define thermoCS  22
#define thermoCLK 23
#define thermoDO  21

// Pressure sensor pin
#define pressurepin 34

// Buzzer pin definition
#define BUZZER_PIN 4

MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);

// PID Setup
double setpoint, input, output, setpointBoot;
double Kp = 48.0, Ki = 8, Kd = 50.0;

PID myPID(&input, &output, &setpoint, Kp, Ki, Kd, DIRECT);

// Pressure Variables
int pressuresetpoint = 9;
int PrePressureSetpoint = 3;
int PressureTarget = pressuresetpoint;
int pumppower = 0;
int maxPressure = 12;
double currentPressure = 0;

// Timing Intervals
const int PRESS_INTERVAL = 50;  // ms
const int PID_INTERVAL = 250;   // ms

// Timer Variables
unsigned long lastPIDTime = 0;
unsigned long DimlastUpdate = 0;
unsigned long LastPressCall = 0;
unsigned long acDetectedTime = 0;
unsigned long elapsedTime = 0; // Shot time in milliseconds
int actime = 0;                // Shot time in seconds
unsigned long lastPublishTime = 0;
unsigned long lastMqttReconnect = 0;
unsigned long lastWifiRetry = 0;

// Steam Variables
String brewTemp;
bool steaming = false;
bool steamMode = false;         // user requested steam mode (NEW, for HA switch)
double steamSetpoint;

// Other Variables
int offset = 9; // Probe offset. If you ask for 100 you get 91, tune this.
int preinftime = 8;
bool pumpPaused = false;        // HA pause/resume override (NEW)

bool acDetected = false;
bool PIDonly = false;
bool shotStarted = false;
bool pumpPowerSetPreinf = false;
bool pumpPowerSetExtraction = false;

int bloomtime = 0;
int offcount = 0;

DimmableLight light(thyristorPin);

// ------------------------------------------------------------------
// Buzzer
// ------------------------------------------------------------------
void beepBuzzer(int times, int beepDuration, int pauseDuration) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(beepDuration);
    digitalWrite(BUZZER_PIN, LOW);
    delay(pauseDuration);
  }
}

// ------------------------------------------------------------------
// Pump output (respects the HA pause override)
// ------------------------------------------------------------------
void setPumpOutput(int value) {
  if (pumpPaused || pumpCutByWeight) {
    light.setBrightness(0);
  } else {
    light.setBrightness(value);
  }
}

// ------------------------------------------------------------------
// PID (unchanged from stock)
// ------------------------------------------------------------------
void runPID() {
  unsigned long PIDnow = millis();
  if ((PIDnow - lastPIDTime >= PID_INTERVAL)) {
    lastPIDTime = PIDnow;
    input = thermocouple.readCelsius();
    if (isnan(input) || input < 0 || input > 160) {
      digitalWrite(SSR_PIN, LOW); // SSR Off
      return;
    }
    myPID.Compute();
    digitalWrite(SSR_PIN, output >= 127 ? HIGH : LOW);
  }
}

// ------------------------------------------------------------------
// Pressure reading (unchanged from stock)
// ------------------------------------------------------------------
void GetPressure() {
  if (!acDetected) { // Pressure sensor is before the solenoid: only read during a shot
    currentPressure = 0;
    return;
  }
  if (elapsedTime < 500) { // Skip first .5s while pressure transients settle
    currentPressure = 0;
    return;
  }
  unsigned long Pnow = millis();
  if (Pnow - LastPressCall >= PRESS_INTERVAL) {
    LastPressCall = Pnow;
    int raw = analogRead(pressurepin);
    currentPressure = (raw * (maxPressure / 4095.0));
    currentPressure = round(currentPressure * 100) / 100.0;
  }
}

// ------------------------------------------------------------------
// Pump power table (unchanged from stock)
// ------------------------------------------------------------------
int basePumpPowerForSetpoint(double Pumpsetpoint) {
  if (Pumpsetpoint <= 3) return 140;
  if (Pumpsetpoint <= 4) return 143;
  if (Pumpsetpoint <= 5) return 147;
  if (Pumpsetpoint <= 6) return 149;
  if (Pumpsetpoint <= 7) return 153;
  if (Pumpsetpoint <= 8) return 155;
  if (Pumpsetpoint <= 9) return 160;
  if (Pumpsetpoint <= 10) return 170;
  return 190;
}

// ------------------------------------------------------------------
// Closed-loop pump pressure control (unchanged from stock, except
// setPumpOutput() so HA pause works mid-shot)
// ------------------------------------------------------------------
void SetPump() {
  if (millis() - DimlastUpdate > PRESS_INTERVAL) {
    DimlastUpdate = millis();
    static int callCount = 0;
    callCount++;
    // Slow adjustment every 4th call (~200ms)
    if (callCount >= 4) {
      callCount = 0;
      if (currentPressure < PressureTarget - 0.2) { pumppower = constrain(pumppower + 1, 120, 255); }
      else if (currentPressure > PressureTarget + 0.2) { pumppower = constrain(pumppower - 2, 120, 255); }
    }
    // Fast cut if overpressure every 50ms
    if (currentPressure > PressureTarget + 0.3) { setPumpOutput(0); }
    else { setPumpOutput(pumppower); }
  }
}

// ------------------------------------------------------------------
// Steam detection (unchanged from stock)
// ------------------------------------------------------------------
void steam() {
  if (input > steamSetpoint - 5 && !steaming) {
    beepBuzzer(3, 100, 100);
    steaming = true;
  }
  if (input < steamSetpoint - 20 && steaming) { steaming = false; }
}

// ------------------------------------------------------------------
// Shot phase label for the dashboard
// ------------------------------------------------------------------
const char* currentShotState() {
  if (!acDetected) return "idle";
  if (preinftime > 0 && actime < preinftime) return "preinfusion";
  if (bloomtime > 0 && actime < preinftime + bloomtime) return "bloom";
  return "extraction";
}

// ------------------------------------------------------------------
// MQTT
// ------------------------------------------------------------------
void publishTelemetry() {
  if (!mqtt.connected()) return;

  StaticJsonDocument<512> doc;
  doc["temp"] = round((input - offset) * 10) / 10.0;
  doc["setpoint"] = round((setpoint - offset) * 10) / 10.0;
  doc["pressure"] = currentPressure;
  doc["pumppower"] = pumppower;
  doc["pressuresetpoint"] = pressuresetpoint;
  doc["preinftime"] = preinftime;
  doc["bloomtime"] = bloomtime;
  doc["actime"] = actime;
  doc["shotstate"] = currentShotState();
  doc["steam"] = steamMode;
  doc["paused"] = pumpPaused;
  doc["Kp"] = Kp;
  doc["Ki"] = Ki;
  doc["Kd"] = Kd;
  doc["PIDonly"] = PIDonly;
  doc["steamsetpoint"] = round((steamSetpoint - offset) * 10) / 10.0;
  doc["weight"] = round(scaleNetWeight() * 10) / 10.0;
  doc["targetweight"] = targetWeight;
  doc["shotweight"] = round(shotWeight * 10) / 10.0;
  doc["bbw"] = bbwEnabled;
  doc["bbwarmed"] = scaleArmed;
  doc["scale"] = scaleConnected ? "connected" : "offline";
  doc["scalestable"] = scaleStable;
  doc["flowrate"] = round(bbwFlowRate * 100) / 100.0;

  String out;
  serializeJson(doc, out);
  mqtt.publish((mqttTopic + "/telemetry").c_str(), out.c_str(), false);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (t.endsWith("/setpoint")) {
    double v = msg.toFloat();
    setpoint = constrain(v + offset, 10 + offset, 96 + offset);
    setpointBoot = setpoint;
  }
  else if (t.endsWith("/pressuresetpoint")) {
    pressuresetpoint = constrain((int)msg.toFloat(), 3, 13);
    pumppower = basePumpPowerForSetpoint(pressuresetpoint);
  }
  else if (t.endsWith("/preinftime")) {
    preinftime = constrain((int)msg.toFloat(), 0, 20);
  }
  else if (t.endsWith("/bloomtime")) {
    bloomtime = constrain((int)msg.toFloat(), 0, 20);
  }
  else if (t.endsWith("/steam")) {
    if (msg.equalsIgnoreCase("ON")) {
      setpoint = steamSetpoint;
      steamMode = true;
    } else {
      setpoint = setpointBoot;
      steamMode = false;
    }
  }
  else if (t.endsWith("/pause")) {
    pumpPaused = msg.equalsIgnoreCase("ON");
    setPumpOutput(pumppower); // applies or clears the pause override
  }
  else if (t.endsWith("/targetweight")) {
    targetWeight = constrain(msg.toFloat(), 0.0f, 100.0f);
  }
  else if (t.endsWith("/bbw")) {
    bbwEnabled = msg.equalsIgnoreCase("ON");
  }
  else if (t.endsWith("/tare")) {
    // Manual tare from the dashboard (zeroes the live weight reading).
    // Declared below scaleTareNow(); forward-declared near the top.
    if (scaleConnected) {
      scaleTareNow();
      Serial.printf("Manual tare (offset %.1fg)\n", scaleTareOffset);
    } else {
      Serial.println("Manual tare ignored - no scale");
    }
  }
  else if (t.endsWith("/kp")) {
    Kp = msg.toFloat();
    myPID.SetTunings(Kp, Ki, Kd);
  }
  else if (t.endsWith("/ki")) {
    Ki = msg.toFloat();
    myPID.SetTunings(Kp, Ki, Kd);
  }
  else if (t.endsWith("/kd")) {
    Kd = msg.toFloat();
    myPID.SetTunings(Kp, Ki, Kd);
  }
  else if (t.endsWith("/pidonly")) {
    PIDonly = msg.equalsIgnoreCase("ON");
  }
  else if (t.endsWith("/steamsetpoint")) {
    steamSetpoint = constrain(msg.toFloat() + offset, 110.0f + offset, 160.0f + offset);
  }
  else if (t.endsWith("/save")) {
    saveConfigToSD();
  }
}

void connectMQTT() {
  if (mqttHost.length() == 0) return;

  if (!mqtt.connected()) {
    if (millis() - lastMqttReconnect < 5000) return;
    lastMqttReconnect = millis();

    String clientId = "discreet-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    String willTopic = mqttTopic + "/status";
    String statusTopic = mqttTopic + "/status";
    String cmdTopic = mqttTopic + "/cmd/#";

    boolean ok;
    if (mqttUser.length() > 0) {
      ok = mqtt.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str(),
                        willTopic.c_str(), 1, true, "offline");
    } else {
      ok = mqtt.connect(clientId.c_str(), willTopic.c_str(), 1, true, "offline");
    }

    if (ok) {
      mqtt.publish(statusTopic.c_str(), "online", true);
      mqtt.subscribe(cmdTopic.c_str());
      publishTelemetry();
      beepBuzzer(1, 100, 100);
      Serial.println("MQTT connected: " + mqttHost);
    } else {
      Serial.println("MQTT connect failed, rc=" + String(mqtt.state()));
    }
  }
  mqtt.loop();
}

// ------------------------------------------------------------------
// SD config (read once at boot, then SD/SPI shut down for good)
// ------------------------------------------------------------------
void loadSDConfig() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card init failed - using defaults");
    beepBuzzer(3, 1000, 200);
    return;
  }

  File configFile = SD.open("/config.json");
  if (configFile) {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, configFile);
    configFile.close();

    ssid = doc["ssid"] | "";
    password = doc["password"] | "";
    mqttHost = doc["mqtt_host"] | "";
    mqttPort = doc["mqtt_port"] | 1883;
    mqttUser = doc["mqtt_user"] | "";
    mqttPass = doc["mqtt_pass"] | "";

    Kp = doc["Kp"] | Kp;
    Ki = doc["Ki"] | Ki;
    Kd = doc["Kd"] | Kd;
    setpoint = doc["setpoint"] | setpoint;
    offset = doc["offset"] | offset;
    steamSetpoint = doc["steamSetpoint"] | steamSetpoint;
    PIDonly = doc["PIDonly"] | PIDonly;
    targetWeight = doc["target_weight"] | targetWeight;
    bbwEnabled = doc["bbw_enabled"] | bbwEnabled;
    cutOnScaleLoss = doc["cut_on_scale_loss"] | cutOnScaleLoss;

    setpoint = setpoint + offset;
    setpointBoot = setpoint;
    steamSetpoint = steamSetpoint + offset;
  } else {
    Serial.println("No /config.json found - using defaults");
    Kp = 80; Ki = 6; Kd = 55;
    setpoint = 93;
    offset = 9;
    setpointBoot = setpoint + offset;
    steamSetpoint = 140 + offset;
    targetWeight = 36.0f;
    bbwEnabled = true;
    cutOnScaleLoss = false;
  }

  // SD + SPI done for the whole boot. Kill both so they never fight WiFi.
  SD.end();
  SPI.end();
  delay(200);
  Serial.println("SD config loaded; SD shut down.");
}

// ------------------------------------------------------------------
// Save current runtime settings to config.json on the SD card
// (MQTT cmd /save). SD is re-initialized for the write, then shut
// down again - the same pattern the boot sequence already uses.
// Temps are written user-facing (offset subtracted), matching the
// stock web UI's saveConfig behaviour.
// ------------------------------------------------------------------
void saveConfigToSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("saveConfig: SD init failed");
    beepBuzzer(3, 500, 200);
    return;
  }

  File configFile = SD.open("/config.json", FILE_WRITE);
  if (!configFile) {
    Serial.println("saveConfig: open failed");
    beepBuzzer(3, 500, 200);
    SD.end();
    SPI.end();
    return;
  }

  StaticJsonDocument<512> doc;
  doc["ssid"] = ssid;
  doc["password"] = password;
  doc["mqtt_host"] = mqttHost;
  doc["mqtt_port"] = mqttPort;
  doc["mqtt_user"] = mqttUser;
  doc["mqtt_pass"] = mqttPass;
  doc["Kp"] = Kp;
  doc["Ki"] = Ki;
  doc["Kd"] = Kd;
  doc["setpoint"] = setpoint - offset;
  doc["offset"] = offset;
  doc["steamSetpoint"] = steamSetpoint - offset;
  doc["PIDonly"] = PIDonly;
  doc["target_weight"] = targetWeight;
  doc["bbw_enabled"] = bbwEnabled;
  doc["cut_on_scale_loss"] = cutOnScaleLoss;

  serializeJson(doc, configFile);
  configFile.close();
  SD.end();
  SPI.end();
  delay(200);

  Serial.println("Config saved to SD");
  beepBuzzer(1, 200, 100);
}

// ------------------------------------------------------------------
// WiFi (unchanged behaviour; removed the gateway/DNS zeroing quirk)
// ------------------------------------------------------------------
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long t0 = millis();
  Serial.println("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    beepBuzzer(5, 1000, 200);
    Serial.println("FAILED connecting to WiFi - will keep retrying");
  } else {
    beepBuzzer(1, 500, 100);
    Serial.println("WiFi Connected! IP: " + WiFi.localIP().toString());
  }
}

// ------------------------------------------------------------------
// BLE scale client + brew-by-weight logic
// ------------------------------------------------------------------
class ScaleScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* adv) override {
    // Match on the FFB0 service OR the advertised name. The scale advertises
    // the 16-bit UUID 0xFFB0, which NimBLE expands to the 128-bit base UUID;
    // isAdvertisingService() handles that comparison correctly.
    // Name matching mirrors GaggiMate's myscalePlugin::handles().
    String nm = String(adv->getName().c_str());
    nm.toLowerCase();
    if (adv->isAdvertisingService(NimBLEUUID(SCALE_SERVICE_UUID)) ||
        nm.indexOf("my_scale") >= 0 ||
        nm.indexOf("blackcoffee") >= 0) {
      scaleFoundAddress = adv->getAddress();
      scaleFound = true;
      NimBLEDevice::getScan()->stop();
    }
  }
};

class ScaleClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* client) override {
    scaleConnected = false;
    Serial.println("Scale disconnected");
  }
};

static ScaleScanCallbacks scaleScanCB;
static ScaleClientCallbacks scaleClientCB;

// ------------------------------------------------------------------
// Weight packet parser (MY_SCALE / Blackcoffee FFB0 protocol)
//
// 20-byte packet, weight is 28-bit big-endian MILLIGRAMS:
//   b0 b1 : AC 40   header / product id (LE u16 0x40AC)
//   b2 hi : sign nibble - 0x8 or 0xC => negative
//   b2 lo : stable flag - 0x1 => settled
//   b3 lo : weight bits 27..24   (b3 HIGH nibble must be masked off)
//   b4    : weight bits 23..16
//   b5    : weight bits 15..8
//   b6    : weight bits 7..0
// Unit-tested host-side in test/test_myscale_parse.c (52 assertions,
// cross-checked against GaggiMate and Bean Conqueror).
// ------------------------------------------------------------------
void scaleNotifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data,
                         size_t len, bool isNotify) {
  if (chr != scaleWeightChr) return;
  if (data == nullptr || len < SCALE_PKT_MINLEN) return;
  if (data[0] != SCALE_HDR0 || data[1] != SCALE_HDR1) return; // not a weight frame

  uint8_t signNib = data[2] >> 4;
  bool isNegative = (signNib == 0x8) || (signNib == 0xC);

  uint32_t mg = ((uint32_t)(data[3] & 0x0F) << 24) |
                ((uint32_t)data[4] << 16) |
                ((uint32_t)data[5] << 8)  |
                ((uint32_t)data[6]);

  scaleRawWeight     = (isNegative ? -1.0f : 1.0f) * ((float)mg / 1000.0f);
  scaleStable        = ((data[2] & 0x0F) == 0x1);
  scaleLastPacketMs  = millis();
}

// ------------------------------------------------------------------
// Hardware tare (VERIFIED byte sequence from GaggiMate myscale.cpp, MIT).
// Bean Conqueror claims this scale cannot tare; GaggiMate ships a working
// tare frame. We send it AND apply a software tare offset.
// ------------------------------------------------------------------
void scaleSendTare() {
  if (!scaleConnected || !scaleCmdChr) return;
  static uint8_t tareCmd[20] = {
    0xAC, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xD2, 0xD2
  };
  scaleCmdChr->writeValue(tareCmd, sizeof(tareCmd), false); // write w/o response
}

// Tare = send the hardware frame, then capture the software offset AFTER a
// short settle window.
//
// Why deferred: we do not know whether the scale honours the hardware tare.
//   - if it DOES, its reading drops to ~0, so the offset must be ~0
//   - if it does NOT, the reading stays at e.g. 312 g, so the offset must be 312
// Capturing the offset immediately would be wrong in the first case (net would
// read about -312 g). Sampling once the dust has settled is correct in BOTH
// cases. The provisional offset keeps net ~0 during the window, and this is
// non-blocking, so the 50 ms control loop is untouched.
void scaleTareNow() {
  scaleSendTare();                          // best effort hardware tare
  scaleTareOffset = scaleRawWeight;         // provisional, correct if HW tare no-ops
  scaleTareSettleAt = millis() + SCALE_TARE_SETTLE_MS;
}

// Called from loop(): finalise a pending tare once the scale has settled.
void scaleServiceTare() {
  if (scaleTareSettleAt == 0) return;
  if ((int32_t)(millis() - scaleTareSettleAt) < 0) return;
  scaleTareOffset = scaleRawWeight;         // authoritative, post-settle
  scaleTareSettleAt = 0;
  Serial.printf("Tare settled (offset %.1fg)\n", scaleTareOffset);
}

void scaleTask(void* param) {
  while (true) {
    if (!scaleConnected) {
      scaleFound = false;
      NimBLEScan* pScan = NimBLEDevice::getScan();
      pScan->setAdvertisedDeviceCallbacks(&scaleScanCB, false);
      pScan->setActiveScan(true);
      pScan->setInterval(97);
      pScan->setWindow(39);
      pScan->start(10, false); // blocks only inside this task

      if (scaleFound) {
        // Reuse a single client object across reconnects. NimBLE 1.4.3 caps
        // simultaneous clients (CONFIG_BT_NIMBLE_MAX_CONNECTIONS, default 3);
        // creating a fresh one per attempt leaks them and eventually fails.
        if (scaleClient == nullptr) {
          scaleClient = NimBLEDevice::createClient();
          scaleClient->setClientCallbacks(&scaleClientCB, false);
          scaleClient->setConnectTimeout(8);
        }
        if (scaleClient->connect(scaleFoundAddress)) {
          NimBLERemoteService* svc =
              scaleClient->getService(NimBLEUUID(SCALE_SERVICE_UUID));
          if (svc) {
            scaleWeightChr = svc->getCharacteristic(NimBLEUUID(SCALE_WEIGHT_UUID));
            scaleCmdChr = svc->getCharacteristic(NimBLEUUID(SCALE_COMMAND_UUID));
            if (scaleWeightChr && scaleWeightChr->canNotify()) {
              scaleWeightChr->subscribe(true, scaleNotifyCallback);
            }
            // FFB1 (command/tare) is optional: software tare covers us if the
            // characteristic is missing, so do not fail the connection on it.
            if (scaleWeightChr) {
              // Request a fast connection interval so weight notifications
              // arrive as quickly as the scale will allow (it pushes ~6.7 Hz).
              // Signature verified against NimBLEClient.h @1.4.3:
              //   updateConnParams(minInterval, maxInterval, latency, timeout)
              // minInterval/maxInterval are in 1.25 ms units, timeout in 10 ms
              // units. 12*1.25 = 15 ms .. 24*1.25 = 30 ms, 0 slave latency,
              // 400*10 ms = 4 s supervision timeout.
              scaleClient->updateConnParams(12, 24, 0, 400);
              scaleRawWeight = 0.0f;
              scaleTareOffset = 0.0f;
              scaleTareSettleAt = 0;      // cancel any pending tare from a prior link
              scaleLastPacketMs = millis();
              scaleConnected = true;
              Serial.println("Scale connected (MY_SCALE FFB0)");
              // Beep ONLY on a genuine new connection, not on every
              // reconnect. This scale deep-sleeps after ~2-3 min of
              // inactivity, which drops the link; without this guard the
              // scan/connect/sleep cycle beeps forever every ~10-15 s.
              // (Reported 2026-08-29: "beeping every 10 seconds".)
              if (!scaleWasEverConnected) {
                scaleWasEverConnected = true;
                beepBuzzer(1, 150, 100);
              }
            } else {
              scaleClient->disconnect();
            }
          } else {
            Serial.println("Scale FFB0 service not found");
            scaleClient->disconnect();
          }
        } else {
          Serial.println("Scale connect failed");
        }
      }
      vTaskDelay(pdMS_TO_TICKS(5000)); // gap between scan rounds
    } else {
      // Stale-data watchdog: the link can stay nominally "up" while the scale
      // deep-sleeps and stops notifying. Treat silence as a disconnect so BBW
      // never cuts on a frozen weight.
      //
      // 3 s was too aggressive: this scale idles/sleeps routinely, so a short
      // timeout caused a permanent scan->connect->sleep->drop cycle (and, with
      // the old unconditional beep, noise every ~10-15 s). 15 s still protects
      // a shot (a 36 g pour takes ~25-30 s, and a frozen reading cannot cause a
      // premature cut because the cut needs weight to RISE to target), while
      // letting an idle scale keep its link.
      if (millis() - scaleLastPacketMs > SCALE_SILENCE_MS) {
        Serial.println("Scale silent - dropping link");
        scaleConnected = false;
        if (scaleClient) scaleClient->disconnect();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
}

// ------------------------------------------------------------------
// Setup / Loop
// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(BUZZER_PIN, OUTPUT);

  loadSDConfig();
  startWiFi();

  // OTA Setup
  ArduinoOTA.setHostname("Discreet");
  ArduinoOTA.setPassword("Discreet");
  ArduinoOTA.begin();

  // Dimmer Setup
  pinMode(SSR_PIN, OUTPUT);
  pinMode(syncPin, INPUT);
  DimmableLight::setSyncPin(syncPin);
  DimmableLight::begin();

  // PID setup
  myPID.SetSampleTime(250);
  myPID.SetOutputLimits(0, 255);
  myPID.SetMode(AUTOMATIC);

  // MQTT setup
  if (mqttHost.length() > 0) {
    mqtt.setServer(mqttHost.c_str(), mqttPort);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(512);
    Serial.println("MQTT target: " + mqttHost + ":" + String(mqttPort));
  } else {
    Serial.println("WARNING: no mqtt_host in config.json - MQTT disabled");
  }

  if (MDNS.begin("discreet")) {
    Serial.println("mDNS started - discreet.local");
  }

  // BLE scale client (brew-by-weight) - runs in its own task so scans and
  // connects never stall the 50 ms pressure loop.
  NimBLEDevice::init("Discreet");
  xTaskCreate(scaleTask, "scaleTask", 8192, nullptr, 1, nullptr);
  Serial.println("BLE scale task started");

  delay(2000);
}

void loop() {
  ArduinoOTA.handle();
  GetPressure();
  runPID();
  steam();
  scaleServiceTare();   // finalise a pending tare once the scale settles

  // AC detection (shot start) - always runs, network-independent
  if (digitalRead(syncPin) == LOW && !acDetected && !PIDonly) {
    acDetectedTime = millis();
    acDetected = true;
  }

  if (acDetected) {

    // Run once at shot start
    if (!shotStarted) {
      shotStarted = true;
      preinftime = (bloomtime > 0 && preinftime < 5) ? 8 : preinftime;

      // Arm brew-by-weight ONLY if the scale is connected right now.
      // (Tare trap: arming mid-shot would tare over already-poured grams
      // and silently move the target.)
      scaleArmed = false;
      shotCutByWeight = false;
      pumpCutByWeight = false;
      shotWeight = 0.0f;
      if (bbwEnabled) {
        if (scaleConnected) {
          scaleTareNow();   // hardware tare (best effort) + software tare offset
          scaleArmed = true;
          Serial.printf("BBW armed (tare offset %.1fg, target %.1fg)\n",
                        scaleTareOffset, targetWeight);
        } else {
          beepBuzzer(2, 300, 150);
          Serial.println("BBW NOT armed - no scale");
        }
      }
    }

    // Calculate shot time
    elapsedTime = millis() - acDetectedTime;
    actime = elapsedTime / 1000;

    // --- PRE-INFUSION ---
    if (preinftime > 0 && actime < preinftime) {
      if (currentPressure <= PrePressureSetpoint - 1) {
        pumppower = 255;
        setPumpOutput(pumppower);
        pumpPowerSetPreinf = false;
      }
      else if (!pumpPowerSetPreinf) {
        PressureTarget = PrePressureSetpoint;
        pumppower = basePumpPowerForSetpoint(PressureTarget);
        setPumpOutput(pumppower);
        pumpPowerSetPreinf = true;
        SetPump();
      }
      else SetPump();
    }

    // --- BLOOM ---
    else if (bloomtime > 0 && actime < preinftime + bloomtime) {
      pumppower = 0;
      setPumpOutput(pumppower);
    }

    // --- EXTRACTION ---
    else {
      if (currentPressure < pressuresetpoint - 2) {
        pumppower = 255;
        setPumpOutput(pumppower);
        pumpPowerSetExtraction = false;
      }
      else if (!pumpPowerSetExtraction) {
        PressureTarget = pressuresetpoint;
        pumppower = basePumpPowerForSetpoint(PressureTarget);
        setPumpOutput(pumppower);
        pumpPowerSetExtraction = true;
        SetPump();
      }
      else SetPump();
    }

    // Brew-by-weight: cut the pump when the cup hits the target weight.
    //
    // Predictive cut: this scale notifies at ~6.7 Hz (150 ms), and there is
    // additional drip/lag after the pump stops. Waiting for net >= target
    // therefore always overshoots. We stop early by the amount we predict
    // will still arrive:  lead = flowRate * BBW_LEAD_TIME_S.
    // Flow rate is measured from the weight curve itself, so it adapts to the
    // actual shot instead of assuming a fixed 2 g/s.
    // Do not evaluate a cut while a tare is still settling: the offset is
    // provisional, so net weight is not yet trustworthy. It is only 400 ms and
    // the cup is empty at that point, so no real pour can be missed.
    if (scaleArmed && !shotCutByWeight && scaleTareSettleAt == 0) {
      float net = scaleNetWeight();
      uint32_t nowMs = millis();

      // Estimate flow rate (g/s) over a ~500 ms window.
      if (bbwLastFlowMs == 0) {
        bbwLastFlowMs = nowMs;
        bbwLastFlowWeight = net;
      } else if (nowMs - bbwLastFlowMs >= 500) {
        float dt = (nowMs - bbwLastFlowMs) / 1000.0f;
        if (dt > 0.0f) {
          float inst = (net - bbwLastFlowWeight) / dt;
          if (inst < 0.0f) inst = 0.0f;          // ignore negative blips
          // Light smoothing so a single noisy packet cannot spike the lead.
          bbwFlowRate = (bbwFlowRate <= 0.0f) ? inst
                                              : (0.6f * bbwFlowRate + 0.4f * inst);
        }
        bbwLastFlowMs = nowMs;
        bbwLastFlowWeight = net;
      }

      // Predicted extra grams that will land after we cut.
      float lead = bbwFlowRate * BBW_LEAD_TIME_S;
      if (lead > BBW_MAX_LEAD_G) lead = BBW_MAX_LEAD_G;  // never cut absurdly early
      if (lead < 0.0f) lead = 0.0f;

      if (net >= (targetWeight - lead)) {
        pumpCutByWeight = true;
        shotCutByWeight = true;
        shotWeight = net;
        setPumpOutput(0);              // cut immediately, don't wait for SetPump()
        beepBuzzer(2, 400, 150);
        Serial.printf("BBW cut at %.1fg (target %.1f, lead %.2f, flow %.2f g/s)\n",
                      net, targetWeight, lead, bbwFlowRate);
      }
    }
    // Scale lost mid-shot: beep and continue manually (the shot keeps
    // running under normal pressure profiling). Optional hard-cut toggle.
    // scaleArmed is only true during a shot, and is cleared here, so this
    // beeps at most once per shot - it cannot join the idle reconnect cycle.
    if (scaleArmed && !scaleConnected) {
      scaleArmed = false;
      Serial.println("Scale lost mid-shot - manual mode");
      beepBuzzer(3, 300, 150);
      if (cutOnScaleLoss) pumpCutByWeight = true;
    }

    // AC turned off
    if (digitalRead(syncPin) == HIGH) offcount++;
    else offcount = 0;

    if (offcount >= 100) {
      acDetected = false;
      shotStarted = false;
      pumpPowerSetPreinf = false;
      pumpPowerSetExtraction = false;
      // Preserve shotWeight so the dashboard keeps showing the last shot's
      // yield after the shot ends; it is reset at the START of the next shot.
      scaleArmed = false;
      shotCutByWeight = false;
      pumpCutByWeight = false;
      scaleTareSettleAt = 0;
      bbwFlowRate = 0.0f;
      bbwLastFlowMs = 0;
      bbwLastFlowWeight = 0.0f;
      offcount = 0;
    }
  }

  // Network (best effort, never blocks the shot loop above)
  if (WiFi.status() == WL_CONNECTED) {
    // MQTT keep-alive + inbound commands
    connectMQTT();

    // Telemetry ~1 Hz
    if (millis() - lastPublishTime >= 1000) {
      lastPublishTime = millis();
      publishTelemetry();
    }
  } else {
    // WiFi dropped - retry without ever touching the control loop
    if (millis() - lastWifiRetry > 10000) {
      lastWifiRetry = millis();
      WiFi.disconnect();
      WiFi.begin(ssid.c_str(), password.c_str());
    }
  }
}
