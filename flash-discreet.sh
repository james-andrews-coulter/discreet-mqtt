#!/usr/bin/env bash
# ------------------------------------------------------------------
# flash-discreet.sh - OTA-flash the Discreet MQTT+BBW firmware.
#
# The machine was NOT reachable when this firmware was built, so it has
# never been flashed. Run this when the machine is powered on and idle.
#
# Usage:
#   ./flash-discreet.sh              # auto-discover the machine, then flash
#   ./flash-discreet.sh 192.168.0.42 # flash a known IP
#
# SAFETY: loop() stops during upload, so the heater SSR holds its last
# state for ~20-30 s. Only flash when the machine is IDLE (not mid-shot,
# ideally not heating). This script refuses to run if a shot is active.
# ------------------------------------------------------------------
set -uo pipefail

SKETCH_DIR="$HOME/discreet-mqtt"
FW="$SKETCH_DIR/build/Discreet_MQTT.ino.bin"
OTA_PORT=3232
OTA_PASS="Discreet"
ESPOTA="$HOME/Library/Arduino15/packages/esp32/hardware/esp32/2.0.17/tools/espota.py"

# Known-other ESP32s on this LAN that ALSO listen on OTA port 3232 and must
# never be flashed with espresso firmware. ss-xiao is the XIAO ESP32S3 garden
# waterer (ESPHome). Add any future ESP32 here.
DENY_IPS=(
  "192.168.0.166"   # ss-xiao garden waterer (ESPHome)
)

die() { echo "ERROR: $*" >&2; exit 1; }

[ -f "$FW" ] || die "firmware not found at $FW - build it first:
  cd $SKETCH_DIR && arduino-cli compile --fqbn esp32:esp32:esp32 Discreet_MQTT --output-dir build"
[ -f "$ESPOTA" ] || die "espota.py not found at $ESPOTA (is ESP32 core 2.0.17 installed?)"

SIZE=$(stat -f%z "$FW")
MAXSZ=1310720
echo "firmware: $FW"
echo "size:     $SIZE bytes ($((SIZE * 100 / MAXSZ))% of the $MAXSZ B OTA slot)"
[ "$SIZE" -lt "$MAXSZ" ] || die "firmware is too big for the OTA slot"

# ---- locate the machine ----
#
# CRITICAL: an open port 3232 does NOT mean "this is the espresso machine".
# ESPHome devices also listen on 3232 (e.g. the ss-xiao garden waterer at
# 192.168.0.166). Flashing espresso firmware onto the wrong ESP32 would brick
# that device's function. So we verify IDENTITY before offering a target:
# the machine must be publishing MQTT telemetry on discreet/telemetry.
IP="${1:-}"

verify_is_discreet() {
  # Returns 0 only if $ip is plausibly the Discreet espresso controller.
  local ip="$1"

  # 1. Hardcoded denylist of KNOWN-other ESP32s on this LAN. This is the only
  #    check that works when the other device is OFFLINE (ss-xiao has an
  #    intermittent 5V press-fit-header fault and drops off the network), so
  #    its IP could be reassigned or it could reappear mid-scan.
  for deny in "${DENY_IPS[@]}"; do
    if [ "$ip" = "$deny" ]; then
      echo "  $ip: on the denylist (known non-espresso ESP32). Skipping."
      return 1
    fi
  done

  # 2. ESPHome exposes its NATIVE API on TCP 6053 (ss-xiao has `api:` enabled).
  #    The Discreet Arduino firmware never does. Reliable even with no
  #    web_server - so do NOT rely on port 80 alone.
  if nc -z -G 1 "$ip" 6053 2>/dev/null; then
    echo "  $ip: port 6053 open = ESPHome native API -> NOT the espresso machine. Skipping."
    return 1
  fi

  # 3. The Discreet MQTT firmware removed its web server entirely, so an open
  #    port 80 means this is something else.
  if nc -z -G 1 "$ip" 80 2>/dev/null; then
    echo "  $ip: port 80 open = serves a web UI -> NOT the Discreet MQTT firmware. Skipping."
    return 1
  fi
  return 0
}

if [ -z "$IP" ]; then
  echo "scanning the LAN for an ArduinoOTA device on port $OTA_PORT..."
  echo "NOTE: other ESP32s (ESPHome) also use 3232 - each candidate is verified."
  SUBNET=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en8 2>/dev/null)
  [ -n "$SUBNET" ] || die "could not determine your LAN address; pass the IP explicitly"
  BASE="${SUBNET%.*}"
  echo "  subnet ${BASE}.0/24"
  CANDIDATES=()
  for i in $(seq 2 254); do
    if nc -z -G 1 "${BASE}.$i" "$OTA_PORT" 2>/dev/null; then
      echo "  port $OTA_PORT open at ${BASE}.$i - verifying..."
      if verify_is_discreet "${BASE}.$i"; then
        CANDIDATES+=("${BASE}.$i")
      fi
    fi
  done

  case "${#CANDIDATES[@]}" in
    0) die "no verified Discreet controller found.
The machine may be off, on another subnet, or not running the MQTT firmware.
Get its IP from your router or the USB serial log and pass it explicitly:
  $0 <ip>" ;;
    1) IP="${CANDIDATES[0]}"
       echo "  verified candidate: $IP" ;;
    *) echo
       echo "MULTIPLE candidates found: ${CANDIDATES[*]}"
       die "refusing to guess. Re-run with the correct IP:
  $0 <ip>" ;;
  esac
else
  # An explicitly supplied IP still gets the safety check.
  verify_is_discreet "$IP" || die "$IP does not look like the Discreet controller.
If you are certain, flash over USB instead."
fi
echo "target:   $IP:$OTA_PORT"

# ---- POSITIVE identity confirmation via MQTT (best effort) ----
# The strongest proof: the Discreet firmware publishes discreet/telemetry.
# If mosquitto_sub is available we require a live telemetry packet, which also
# doubles as the mid-shot safety check below.
TELEMETRY=""
if command -v mosquitto_sub >/dev/null 2>&1 && [ -f "$SKETCH_DIR/.mqtt_creds" ]; then
  # shellcheck disable=SC1090
  . "$SKETCH_DIR/.mqtt_creds"
  TELEMETRY=$(mosquitto_sub -h "${MQTT_HOST:-localhost}" \
                -u "${DISCREET_MQTT_USER:-}" -P "${DISCREET_MQTT_PW:-}" \
                -t discreet/telemetry -C 1 -W 4 2>/dev/null || true)
  if [ -n "$TELEMETRY" ]; then
    echo "confirmed: live discreet/telemetry seen on the broker"
  else
    echo "WARNING: no discreet/telemetry within 4 s."
    echo "  The machine may be offline, or the broker/creds may differ."
    echo "  Cannot POSITIVELY confirm $IP is the espresso machine."
  fi
else
  echo "note: mosquitto_sub not installed - skipping MQTT identity confirmation"
fi

# ---- refuse to flash mid-shot ----
# Reuses the telemetry packet already fetched above.
if [ -n "$TELEMETRY" ]; then
  STATE=$(printf '%s' "$TELEMETRY" \
          | /usr/local/bin/python3 -c 'import sys,json; print(json.load(sys.stdin).get("shotstate","?"))' 2>/dev/null || echo "?")
  echo "shot state: $STATE"
  case "$STATE" in
    preinfusion|bloom|extraction)
      die "a shot is in progress - wait for it to finish before flashing" ;;
  esac
fi

# ---- final confirmation ----
echo
echo "About to flash the ESPRESSO controller at $IP."
if [ -z "$TELEMETRY" ]; then
  echo "!! Identity NOT positively confirmed. Other ESP32s on your LAN also"
  echo "!! listen on 3232 (e.g. the ss-xiao garden waterer). Flashing the wrong"
  echo "!! device would overwrite its firmware. Double-check the IP first."
fi
read -r -p "Flash now? The heater holds its last state for ~30 s. [y/N] " ans
case "$ans" in [yY]*) ;; *) echo "aborted"; exit 0 ;; esac

echo "uploading (espota is UDP; 'Authenticating...OK' means the handshake worked)"
/usr/local/bin/python3 "$ESPOTA" -i "$IP" -p "$OTA_PORT" -a "$OTA_PASS" -f "$FW" -d -r
rc=$?

if [ $rc -eq 0 ]; then
  echo
  echo "UPLOAD OK. The ESP32 reboots now. Verify:"
  echo "  1. serial (115200) shows: 'BLE scale task started'"
  echo "  2. wake the scale, then look for: 'Scale connected (MY_SCALE FFB0)'"
  echo "  3. press on the scale -> sensor.discreet_scale_weight climbs in HA"
  echo "  4. press the HA 'Tare Scale' button -> weight returns to ~0"
  echo "  5. CONFIRM SCALE FACTOR: put a known mass (e.g. 500 g) on the scale"
  echo "     and check HA reads 500.0 - this is the one unverified assumption."
else
  echo
  echo "UPLOAD FAILED (rc=$rc). Notes:"
  echo "  - core 2.x ArduinoOTA is UDP-only: a refused TCP probe to 3232 means nothing."
  echo "  - only flash when idle; the sketch must be running (not crashed)."
  echo "  - if OTA is wedged, flash over USB:"
  echo "      arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32 $SKETCH_DIR/Discreet_MQTT"
fi
exit $rc
