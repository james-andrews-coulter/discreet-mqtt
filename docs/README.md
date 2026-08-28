# docs/ — reference material

## `my-scale-ble-protocol.md`
**Start here.** The verified BLE protocol for James's actual scale
(`MY_SCALE`, Blackcoffee/FFB0), captured live from the device on
2026-08-28: full GATT table, packet byte tables, decode, tare, deep-sleep
behaviour, and open questions.

## `upstream/`
Reference implementations of the same protocol, downloaded unmodified.
Both were used to cross-check this firmware's decode; the firmware's parser
reproduces GaggiMate's integer output exactly.

- `myscale.cpp` / `myscale.h` — GaggiMate `esp-arduino-ble-scales`
  (**MIT licensed**, so safe to reference from a GPLv3 project). This is
  the closest thing to canonical: production ESP32 C++ + NimBLE for this
  exact scale, including the hardware tare frame.
  Source: https://github.com/gaggimate/esp-arduino-ble-scales

## `blackcoffeeScale.ts`

Bean Conqueror's driver for the same scale (it matches on device names
`blackcoffee` and `my_scale`). Note it indexes the packet's **hex string by
nibble** (`hex.slice(7,14)`), not by byte — easy to misread. It also
declares `supportsTaring = false`, which GaggiMate contradicts.
Source: https://github.com/graphefruit/Beanconqueror

## `nimble-1.4.3-api-reference.json`

Compile-accurate NimBLE-Arduino **1.4.3** API notes, extracted by reading the
actual 1.4.3 headers locally (not from prose docs — 1.4.x and 2.x differ in
ways that silently fail to compile). Covers `updateConnParams` signature and
units, the FreeRTOS task pattern, dual-core thread-safety of shared variables,
the reconnect pattern and max-client limit, `init()` order vs WiFi, 16-bit vs
128-bit UUID matching, and flash-size reduction options.

Two findings from it are load-bearing in the firmware:
- `updateConnParams(minInterval, maxInterval, latency, timeout)` — intervals in
  **1.25 ms** units, timeout in **10 ms** units.
- `isAdvertisingService()` correctly matches a 16-bit advertised UUID (`0xFFB0`)
  against its 128-bit expansion, because `NimBLEUUID::operator==` promotes to
  the Bluetooth base UUID before comparing. This is why scanning by service
  UUID works for this scale.

## `OBSOLETE-Discreet_MQTT.ino.core3-nimble25`
**Do not use.** An old ESP32-core-3.x / NimBLE-2.x variant of the sketch,
kept only for reference. Two reasons it is dead:
1. it targets the **wrong scale protocol** (Bean Conqueror `6E4000xx`
   UUIDs, which this scale does not speak), and
2. it does not fit — core 3.x + NimBLE 2.x produced a 1.33 MB binary
   against a 1.25 MB OTA slot, and core 3.x broke the dimmer library's
   timer API.

The supported toolchain is ESP32 core **2.0.17** + NimBLE-Arduino **1.4.3**.
