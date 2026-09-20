# Discreet -> Home Assistant (MQTT conversion + brew-by-weight)

Replaces the stock Discreet web UI (served from SD card) with a Home
Assistant dashboard, and adds **brew-by-weight** using a `MY_SCALE`
Bluetooth kitchen scale. The ESP32 talks to HA over MQTT and to the scale
over BLE. No custom HA components, no 30-second polling, live 1 Hz
telemetry.

---

## 🚀 Quick Start (5 minutes reading, then ~2 hours hands-on)

**New to this? Start here.** This project lets you control a Discreet espresso machine from Home Assistant with a Bluetooth scale that auto-stops the shot at your target weight.

### What you need to buy

| Item | Qty | Notes |
|------|-----|-------|
| Discreet espresso machine | 1 | The hardware this runs on |
| ESP32 DevKit v1 (classic ESP32, **not** S2/S3/C3) | 1 | Must fit inside the machine |
| MAX6675 thermocouple module | 1 | Reads boiler temperature |
| Fotek SSR-25 DA (or equivalent) | 1 | Switches the heating element |
| `MY_SCALE` Bluetooth kitchen scale | 1 | **Exact model only** — WeighMyBru, Bean Conqueror, etc. will NOT work |
| MicroSD card (FAT32) | 1 | For config.json |
| USB-C power supply (5 V / 2 A) | 1 | Powers the ESP32 |
| Jumper wires / soldering gear | - | See wiring in `docs/upstream/` |

### What you need already running

- **Home Assistant** (any install: HAOS, Docker, supervised)
- **Mosquitto MQTT broker** add-on (in HA: Settings → Add-ons → Mosquitto broker)
- **MQTT integration** enabled in HA (Settings → Devices & Services → MQTT)

### The 4 steps (do in order)

1. **Wire the hardware** — see `docs/upstream/` for the GaggiMate schematic. ESP32 GPIOs: thermocouple (19/23/18), SSR (25).
2. **Flash the firmware** — see [§1 Firmware](#1-firmware) below. Use `./flash-discreet.sh` after first USB flash.
3. **Install the HA package** — drop `packages/discreet.yaml` into `<ha-config>/packages/`, add `packages: !include_dir_named packages` to `configuration.yaml`, restart HA.
4. **Import the dashboard** — Settings → Dashboards → Add Dashboard → Raw configuration editor → paste `dashboard.yaml`.

### First shot checklist

- [ ] Scale shows `Scale connected (MY_SCALE FFB0)` in serial monitor (115200 baud)
- [ ] Press **Tare Scale** in HA → weight reads ~0 g
- [ ] Put known mass (500 g) on scale → HA reads 500.0 g (confirms scale factor)
- [ ] Wake scale, put cup on, pull shot → auto-cuts at target weight

> ⚠️ **The scale deep-sleeps after ~2 minutes.** Wake it (press button) before every shot or brew-by-weight won't arm.

---

## Architecture (two brains, one control surface)

- **ESP32 inside the machine** (this firmware): PID, pressure profiling,
  shot logic, and the BLE *central* that reads the scale. Nothing about
  the machine depends on the scale - forget the scale and it's exactly
  the manual machine you have today.
- **BLE scale on the drip tray**: standalone, battery-powered, stock
  firmware. It notifies weight over BLE and the ESP32 reads it.
- **Home Assistant**: the dashboard. Sets targets, shows state, records
  history. The shot loop itself always runs on the ESP32.

## Files (what each one does)

| File | Purpose | You touch it? |
|------|---------|---------------|
| `Discreet_MQTT.ino` | Firmware (runs on ESP32). Root file is a symlink — edit `Discreet_MQTT/Discreet_MQTT.ino`. | Yes (WiFi/MQTT config) |
| `packages/discreet.yaml` | MQTT entity definitions for HA. Drop into `<ha-config>/packages/`. | No (drop in) |
| `dashboard.yaml` | Lovelace dashboard. Import via Raw Configuration Editor. | No (import) |
| `config.json.example` | Template for SD card config. Copy to `config.json` on SD card. | Yes (fill in) |
| `.mqtt_creds.example` | Template for flash script MQTT creds. Copy to `.mqtt_creds`. | Yes (fill in) |
| `flash-discreet.sh` | Safe OTA flasher with identity checks. Run when machine is idle. | Run it |
| `docs/my-scale-ble-protocol.md` | Verified BLE protocol docs (for debugging). | Reference only |
| `docs/upstream/` | Reference schematics & code (GaggiMate). | Wiring reference |
| `test/` | Host-side unit tests (run `cd test && ./run-tests.sh`). | Run once to verify |

## THE SCALE (read this first)

The scale in this build is **not** a WeighMyBru and does **not** speak the
Bean Conqueror / Nordic-UART protocol. It was identified by live GATT
enumeration on 2026-08-28:

| | |
|---|---|
| Advertised name | `MY_SCALE` |
| Service | `0000FFB0-0000-1000-8000-00805F9B34FB` |
| Weight notify char | `0000FFB2-...` (20-byte packets, ~6.7 Hz) |
| Command write char | `0000FFB1-...` (write-without-response) |
| Chipset | TI CC254x (exposes the TI OAD service `F000FFC0`) |

Weight is a **28-bit big-endian value in milligrams** spanning
`(byte3 & 0x0F), byte4, byte5, byte6`, with the sign in the high nibble of
byte 2 and a stable/settled flag in its low nibble. Packets start `AC 40`.

This protocol is supported by two independent open-source projects, and
this firmware's decode was cross-checked against **both**:

- GaggiMate `esp-arduino-ble-scales/src/scales/myscale.cpp` (MIT)
- Bean Conqueror `src/classes/devices/blackcoffeeScale.ts` (as
  `blackcoffee` / `my_scale`)

Full byte tables, the live capture, and citations: `docs/my-scale-ble-protocol.md`.

### Taring

Bean Conqueror declares this scale cannot tare. GaggiMate ships a working
tare frame (`AC 40 00 ... 00 D2 D2`). This firmware does **both**, because
we cannot know in advance which is true for your unit:

1. sends the hardware tare frame, then
2. **400 ms later** records a **software tare offset** - the reading at
   that moment is subtracted from everything after it.

The delay matters. If the offset were sampled immediately and the scale
*did* honour the frame, the reading would drop to ~0 while the offset still
held the old cup weight, so net weight would read about **-312 g** and the
shot would never cut. Sampling after a settle window is correct in *both*
cases:

| | scale honours tare | scale ignores tare |
|---|---|---|
| reading at sample time | ~0 g | 312 g (cup) |
| offset recorded | ~0 g | 312 g |
| net weight | 0 g ✓ | 0 g ✓ |

It is non-blocking (serviced from `loop()`, never a `delay()`), and the cut
logic is suppressed while a tare is settling. `weight` in telemetry is
always the *net* (tared) value. Regression-tested in
`test/test_myscale_parse.c` section 8.

## 1. Firmware

1. Open `Discreet_MQTT.ino` in Arduino IDE (or PlatformIO).
2. Toolchain that is VERIFIED to compile this sketch:
   - ESP32 Arduino core **2.0.17** (NOT 3.x - binary overflows the OTA slot)
   - **NimBLE-Arduino 1.4.3** (NOT 2.x - API differences)
   - PubSubClient, Dimmable Light for Arduino 1.6.0, MAX6675 (Rob Tillaart),
     PID (Brett Beauregard), ArduinoJson
   - CLI: `arduino-cli compile --fqbn esp32:esp32:esp32 Discreet_MQTT`
3. Add the MQTT + scale keys to `config.json` on the SD card:

```json
{
  "ssid": "your_wifi_name",
  "password": "your_wifi_password",
  "mqtt_host": "192.168.1.50",
  "mqtt_port": 1883,
  "mqtt_user": "mqttuser",
  "mqtt_pass": "mqttpass",
  "target_weight": 36.0,
  "bbw_enabled": true,
  "cut_on_scale_loss": false
}
```

   - `target_weight` - grams to auto-cut at (default 36)
   - `bbw_enabled` - master switch for brew-by-weight (default true)
   - `cut_on_scale_loss` - `true` = hard-cut the pump if the scale dies
     mid-shot (accept a possibly-short shot); `false` (default) = beep
     and continue manually. Existing Kp/Ki/Kd/setpoint/offset keys still
     load as before.
4. Flash via USB, later via ArduinoOTA (hostname `Discreet`, password
   `Discreet`).

> **MQTT credentials for flash-discreet.sh** (optional but recommended):
> copy `.mqtt_creds.example` to `.mqtt_creds` and fill in your broker
> host/user/pass. The script uses this to positively confirm the target
> machine via live `discreet/telemetry` before flashing. Without it, the
> script falls back to mDNS + port checks only.

> BUILD STATUS (Aug 29, 2026): **FLASHED SUCCESSFULLY** to 192.168.x.x (yours)
> via OTA (`Authenticating...OK` -> 100% -> `Result: OK`). Confirmed alive
> after reboot: pings clean, and mDNS re-advertising `_arduino._tcp` proves
> `loop()` is running (a crashed sketch stops advertising).
> Built with arduino-cli 1.5.1 + ESP32 core **2.0.17** + NimBLE **1.4.3**.
> Binary 1,142,592 B = **87%** of the 1.25 MB OTA slot. RAM 60,488 B (18%).
>
> **Scale factor CONFIRMED** (was the one unverified assumption): a real
> calibrated load on the scale decoded to **481.790 g**, so the milligram
> factor (`/1000`) is correct. The negative-sign path was also confirmed
> live at **-481.83 g** for the same object (sign symmetry within 40 mg).
>
> **NOT yet verified on hardware** - do NOT trust it on a real shot until
> these are checked (see "Bench test"): the ESP32-side BLE link to the
> scale (only ever exercised from macOS/bleak), the deferred tare, and the
> predictive cut firing during an actual extraction. No MQTT broker was
> running on the LAN, so telemetry could not be read back.

### ⚠ Finding the machine: ArduinoOTA discovery is UDP, not TCP

**Do not scan for the machine with `nc -z <ip> 3232`.** On ESP32 core 2.x
ArduinoOTA's discovery port is **UDP**, so a TCP connect probe reports
"closed" on a machine that is perfectly reachable and flashable. That false
negative wasted hours here - repeated LAN sweeps concluded "the machine is
offline" while it was sitting at `192.168.x.x (yours)` answering pings.

Use **mDNS** instead, which is what `flash-discreet.sh` now does:

```sh
ping -c1 discreet.local          # the firmware sets hostname "discreet"
dns-sd -B _arduino._tcp local    # browse for any ArduinoOTA node
dns-sd -L discreet _arduino._tcp local   # TXT record: board=esp32
```

The `_arduino._tcp` TXT record is also a useful identity check: it reports
`board=esp32`.

### ⚠ Flashing: do NOT just pick the first device on port 3232

**Other ESP32s on this LAN also listen on OTA port 3232.** Ensure you target the espresso machine, not another ESP32 on your LAN. A naive "scan for 3232 and flash it" would overwrite that device's firmware.

`flash-discreet.sh` therefore verifies identity before offering a target:

1. **denylist** of known-other ESP32 IPs - the only check that still works when that device is offline;
2. **port 6053 closed** - ESPHome exposes its native API there, the Discreet Arduino firmware never does;
3. **port 80 closed** - the MQTT conversion removed the web server;
4. **positive confirmation** - requires a live `discreet/telemetry` packet on
   the broker (also reused for the mid-shot safety check). If it cannot be
   confirmed, the script says so loudly before the final prompt;
5. refuses to guess if multiple candidates survive.

If you add another ESP32 to the LAN, add its IP to `DENY_IPS` in the script (or override via `$DISCREET_DENY_IPS`).


## Predictive cut (why yield lands on target)

Naively cutting when `weight >= target` always overshoots: the scale only
notifies every 150 ms, and the puck keeps dripping after the pump stops.

This firmware measures the **actual flow rate** from the weight curve and
cuts early by `flowRate * BBW_LEAD_TIME_S` (default 0.35 s), clamped to
`BBW_MAX_LEAD_G` (3 g) so a noisy reading can never cut absurdly early.

Simulated over realistic shot curves (`test/test_bbw_logic.c`):

| Shot | Naive settled | Predictive settled |
|---|---|---|
| 36 g target, 2.0 g/s, 0.7 g drip | 37.00 g (+1.00) | **36.05 g (+0.05)** |
| gusher 3.5 g/s, 1.2 g drip | - | 36.30 g (+0.30) |
| choked 0.8 g/s, 0.3 g drip | - | 36.04 g (+0.04) |

**Tuning:** if your shots consistently overshoot, raise `BBW_LEAD_TIME_S`;
if they come up short, lower it. It's a single `#define` near the top of
the sketch.

## Tests (run these - they need no hardware)

```
cd test && ./run-tests.sh
```

Compiles both suites with `-Wall -Wextra -Werror -O2` and runs them.

- `test_myscale_parse.c` - **75 assertions**. Parses the REAL captured
  packets, cross-checks our decode against GaggiMate's and Bean
  Conqueror's implementations on every case, and covers sign nibbles,
  stability flag, malformed/hostile input, the byte-3 nibble mask, the
  deferred-tare regression, the falsified trailer-checksum hypotheses, and
  the **live-captured ground-truth packets** (section 11).
- `test_bbw_logic.c` - **19 assertions**. Replays realistic shot curves
  through the exact cut algorithm; asserts yield accuracy, the lead
  clamp, and every safety rule (unarmed never cuts, cut latches once,
  negative weight never cuts, missing cup never cuts).

Both suites pass: **105/105**.

> Note on the trailer bytes: GaggiMate defines a `calculateChecksum()`
> ("sum of all bytes except the last") but never calls it. Do **not** add
> checksum validation — tested against the real packet, every sum/XOR
> hypothesis fails, so validating would reject every genuine packet and the
> scale would look dead. Test section 9 locks this in. Validity = `AC 40`
> header + length.

## 2. Bench test the scale (do this before trusting a shot)

Serial monitor at 115200 should show, in order:

```
BLE scale task started
Scale connected (MY_SCALE FFB0)      <- after you WAKE the scale
```

**The scale deep-sleeps aggressively** (verified: it stopped advertising
within ~2-3 min of idle, repeatedly, and is completely undiscoverable
once asleep). Wake it before a shot or BBW simply won't arm.

Checks worth doing once:
- press on the scale -> `weight` climbs in MQTT telemetry
- press the **Tare Scale** button in HA -> weight returns to ~0
- real pull -> `BBW armed (tare offset X, target Y)` at shot start,
  `BBW cut at 36.0g (target 36.0, lead 0.70, flow 2.00 g/s)` at the cut
- scale off at shot start -> `BBW NOT armed - no scale` + warning beeps,
  shot runs manual
- scale dies mid-shot -> `Scale silent >3s - dropping link` then
  `Scale lost mid-shot - manual mode`, shot continues, no stale-weight cut
- cup missing -> weight ~0, no cut, obvious on the dashboard

## 3. Home Assistant

1. Add the **Mosquitto broker** add-on; enable the MQTT integration.
2. In your HA `configuration.yaml`, ensure packages are enabled:
   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```
   Drop `packages/discreet.yaml` into `<ha-config>/packages/discreet.yaml` and restart Home Assistant.
3. Reload MQTT / wait ~1 min: entities appear, including `sensor.discreet_scale_weight`, `sensor.discreet_shot_weight`, `sensor.discreet_flow_rate`, `binary_sensor.discreet_scale_connected`, `binary_sensor.discreet_bbw_armed`, `binary_sensor.discreet_scale_stable`, `number.discreet_target_weight`, `switch.discreet_brew_by_weight`, `button.discreet_tare_scale`.
4. Import the dashboard:
   - `Settings → Dashboards → Add Dashboard`
   - Name it (e.g. `Discreet`), open it
   - Click `⋮` (top right) → `Raw configuration editor`
   - Delete placeholder content
   - Paste full contents of `dashboard.yaml` → `Save`
   - Confirm 3 views (Main, Settings, Statistics) render cleanly.

The Main view gains a big gold **Weight** readout with a status line that
reads `Armed - auto-stop at 36 g`, `Scale ready`, `No scale - manual
shot`, or `BBW Off`, plus a Brew by Weight control block and a
weight/flow history graph.


## MQTT topic reference

| Topic                          | Direction   | Payload                            |
|--------------------------------|-------------|------------------------------------|
| `discreet/status`              | ESP32 -> HA | `online` / `offline` (LWT)         |
| `discreet/telemetry`           | ESP32 -> HA | JSON, 1 Hz (below)                 |
| `discreet/cmd/setpoint`        | HA -> ESP32 | brew temp °C e.g. `93.5`           |
| `discreet/cmd/pressuresetpoint`| HA -> ESP32 | bar, `3`-`13`                      |
| `discreet/cmd/preinftime`      | HA -> ESP32 | seconds `0`-`20`                   |
| `discreet/cmd/bloomtime`       | HA -> ESP32 | seconds `0`-`20`                   |
| `discreet/cmd/steam`           | HA -> ESP32 | `ON` / `OFF`                       |
| `discreet/cmd/pause`           | HA -> ESP32 | `ON` / `OFF`                       |
| `discreet/cmd/targetweight`    | HA -> ESP32 | grams `0`-`100` e.g. `36.5`        |
| `discreet/cmd/bbw`             | HA -> ESP32 | `ON` / `OFF` (master switch)       |
| `discreet/cmd/tare`            | HA -> ESP32 | any payload - tare the scale now   |
| `discreet/cmd/kp`              | HA -> ESP32 | PID Kp (applied live via SetTunings)|
| `discreet/cmd/ki`              | HA -> ESP32 | PID Ki (applied live via SetTunings)|
| `discreet/cmd/kd`              | HA -> ESP32 | PID Kd (applied live via SetTunings)|
| `discreet/cmd/pidonly`         | HA -> ESP32 | `ON`/`OFF` - PID-only mode (disables shot logic) |
| `discreet/cmd/steamsetpoint`   | HA -> ESP32 | steam target temp °C (110-160)     |
| `discreet/cmd/save`            | HA -> ESP32 | any payload - writes current runtime settings to config.json on SD (survives reboot) |

Telemetry JSON: `temp`, `setpoint`, `pressure`, `pumppower`,
`pressuresetpoint`, `preinftime`, `bloomtime`, `actime`, `shotstate`,
`steam`, `paused`, `Kp`, `Ki`, `Kd`, `PIDonly`, `steamsetpoint`, plus
scale fields: `weight` (live NET grams), `targetweight`, `shotweight`
(net grams at the cut), `bbw` (enabled), `bbwarmed`, `scale`
(`connected`/`offline`), `scalestable`, `flowrate` (g/s).

## Brew-by-weight rules

1. **Scale off / asleep at shot start** -> normal manual shot, warning
   beeps, dashboard shows scale offline. Never blocks or delays a shot.
2. **Scale connects mid-shot** -> BBW does NOT arm (the tare offset would
   be wrong, silently moving the target). It only arms when connected
   AND tared at shot start.
3. **Scale dies mid-shot** -> loud beeps, shot continues manually. No
   cut on stale weight by default; `cut_on_scale_loss: true` opts into
   hard safety. A **stale-data watchdog** drops the link after 3 s of
   silence, so a sleeping scale can never freeze the weight and trigger
   a bogus cut.
4. **Cup not on scale** -> weight reads ~0, no cut, obvious on the
   dashboard while the shot runs.
5. The auto-tare at shot start means you never touch the scale: wake it,
   put the cup on, pull the shot.

## Caveats

- **Shot control stays on the ESP32.** HA and the scale only monitor and
  set targets. The 50 ms pressure loop, pre-infusion/bloom sequencing
  and the weight cut all run on-device - same architecture as Gaggiuino.
- **BLE scanning vs WiFi**: scanning runs in its own FreeRTOS task so it
  can never stall the control loop. BLE/WiFi coexist fine on the classic
  ESP32; you may see slight WiFi latency blips during scans, harmless.
- **Scale must be awake.** It deep-sleeps within a couple of minutes.
- **Temp number range** (80-96 °C) is exact at the default `offset` of 9.
- **NimBLE-Arduino version**: 2.x needs Arduino core 3.x, 1.4.x needs
  core 2.x. The dimmer library pins the core to 2.x, so use NimBLE 1.4.3.
- A single BLE client object is reused across reconnects: NimBLE 1.4.3
  caps simultaneous clients, and creating one per attempt leaks them.

## Hardware requirements

| Component | Spec / Notes |
|-----------|--------------|
| ESP32 board | Classic ESP32 (not S2/S3/C3). Tested on ESP32 DevKit v1 / ESP-WROOM-32. Must fit inside the Discreet enclosure. |
| MAX6675 | Thermocouple amplifier (SPI). Wired to GPIO 19 (MISO), 23 (SCK), 18 (CS). |
| SSR | Fotek SSR-25 DA or equivalent (3-32 VDC control, 240 VAC load). Driven from GPIO 25 (active-high). |
| Scale | `MY_SCALE` (TI CC254x, service `0000FFB0...`). **Must** be this exact model; WeighMyBru / Bean Conqueror scales use a different protocol and will not work. |
| SD card | MicroSD, formatted FAT32. Holds `config.json` at root. |
| Power | 5 V / 2 A USB-C supply to the ESP32 (the Discreet board regulates internally). |

Wiring matches the original Discreet hardware. See `docs/upstream/` for the GaggiMate reference schematic.

## SD card layout

Insert the SD card into the ESP32 board **before first boot**. The firmware expects `config.json` at the **root** of the card:

```
/config.json        <- your WiFi + MQTT + PID settings
```

The firmware will create/overwrite this file when you press **Save Config** in HA (writes current runtime settings to survive reboot).

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `discreet.local` doesn't resolve | mDNS not working / machine on different subnet | Find IP in router DHCP table, flash with `./flash-discreet.sh <ip>` |
| OTA upload hangs at "Authenticating..." | Wrong IP (flashing another ESP32) / sketch crashed | Verify identity checks pass; if sketch crashed, flash over USB |
| Scale never connects (`Scale connected` never appears) | Scale asleep / wrong scale model / BLE interference | Wake scale (press button); confirm it's `MY_SCALE`; move other BLE devices away |
| Weight reads ~0 g with cup on scale | Tare not run / scale factor wrong | Press **Tare Scale** in HA; verify with known mass (500 g) |
| Shot auto-cuts way early / way late | `BBW_LEAD_TIME_S` needs tuning | Adjust the `#define` in the sketch (default 0.35 s); see Predictive cut section |
| Entities missing in HA after package install | Packages not enabled / HA not restarted | Add `packages: !include_dir_named packages` to `configuration.yaml`, restart HA |
| `flash-discreet.sh` says "could not find the Discreet controller" | Machine off / crashed / different subnet | Ping `discreet.local`; check router; flash over USB if needed |

## Common pitfalls

- **Do not use ESP32 Arduino core 3.x** — binary exceeds OTA slot. Core **2.0.17** is required.
- **Do not use NimBLE-Arduino 2.x** — API incompatible. **1.4.3** is required.
- **The scale deep-sleeps aggressively** (~2-3 min). Wake it before every shot or BBW won't arm.
- **MQTT broker must be running** before the ESP32 boots, or it will retry forever (harmless but noisy).
- **Only one BLE client** is created and reused. Reconnect logic handles drops; do not power-cycle the scale mid-shot.

## Not yet done

- **Bench-untested:** ESP32-side BLE link + deferred tare + predictive cut during a live extraction (only exercised from macOS/bleak so far).
- `FFB1` command bytes beyond tare, and the byte 18-19 checksum, remain
  undocumented. Neither is needed.

## System diagram

```
┌─────────────────┐     BLE (6.7 Hz)      ┌──────────────┐
│  ESP32 inside   │ ◄───────────────────── │  MY_SCALE    │
│  the machine    │   weight notifications │  (on drip    │
│                 │   tare command         │   tray)      │
│  • PID loop     │                        └──────────────┘
│  • Pressure     │
│  • Shot logic   │
│  • BLE central  │
└────────┬────────┘
         │ MQTT (1 Hz telemetry, commands)
         ▼
┌─────────────────┐
│  Mosquitto MQTT │
│  broker (HA)    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Home Assistant │
│  • Entities     │
│  • Dashboard    │
│  • Automations  │
└─────────────────┘
```

## Where to get help

- **Issues:** [GitHub Issues](https://github.com/james-andrews-coulter/discreet-mqtt/issues) — bugs, questions, feature requests
- **Scale protocol:** `docs/my-scale-ble-protocol.md` — full byte tables + live capture
- **Wiring reference:** `docs/upstream/` — GaggiMate schematic (MIT licensed)
- **Original Discreet:** https://github.com/Discreet-Coffee/Discreet

## License


GPLv3 — derivative of https://github.com/Discreet-Coffee/Discreet (GPLv3).
Scale decode cross-checked against GaggiMate esp-arduino-ble-scales (MIT, see docs/upstream/) and BeanConqueror (GPLv3, see docs/blackcoffeeScale.ts).

