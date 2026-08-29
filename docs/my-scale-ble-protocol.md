# MY_SCALE (Blackcoffee-protocol) BLE scale — VERIFIED live capture

Captured 2026-08-28 23:36 EEST from James's actual scale, on macOS via bleak.
This supersedes the earlier assumption that the scale is a WeighMyBru speaking
the Bean Conqueror / Nordic-UART (6E4000xx) protocol. **It is not.**

## Identity (VERIFIED — live scan)

| Field | Value |
|---|---|
| Advertised name | `MY_SCALE` |
| Advertised service | `0000ffb0-0000-1000-8000-00805f9b34fb` (16-bit `0xFFB0`) |
| Manufacturer data ID | `16556` = `0x40AC` |
| Manufacturer payload | `6d4e0a005cd0` |
| BLE address (macOS UUID) | `88183D0C-A1A6-B897-8B7D-0069103CD8A4` |
| RSSI at bench | ~ -50 dBm |

macOS hides real MAC addresses behind a CoreBluetooth UUID. The ESP32 will see
the true public/random MAC — so the firmware must match on **name or service
UUID**, never on this UUID.

## Full GATT table (VERIFIED — enumerated live)

```
SERVICE 0000180a-...  (Device Information)
   CHAR 00002a24  read     Model Number
   CHAR 00002a27  read     Hardware Revision
   CHAR 00002a26  read     Firmware Revision
   CHAR 00002a28  read     Software Revision
SERVICE 0000ffb0-0000-1000-8000-00805f9b34fb   <-- SCALE DATA SERVICE
   CHAR 0000ffb1-...  write-without-response      <-- command (write only)
   CHAR 0000ffb2-...  notify, read                <-- WEIGHT NOTIFY
SERVICE f000ffc0-0451-4000-b000-000000000000     (TI OAD firmware-update service)
   CHAR f000ffc1  write, notify, write-without-response
   CHAR f000ffc2  write, notify, write-without-response
```

The `f000ffc0` service is the **Texas Instruments OAD (Over-the-Air Download)**
service — this identifies the scale as a TI CC2540/CC2541-class BLE chipset.
Do not write to it; it is the firmware-flash endpoint.

## Weight notification format (VERIFIED structure, decode CROSS-CHECKED to Bean Conqueror)

Notifications arrive on `0000ffb2` at a measured **~6.7 Hz** (150 ms interval),
20 bytes per packet. Observed idle/zero packet:

```
ac 40 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 a6 a7
 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19
```

| Bytes | Meaning |
|---|---|
| 0-1 | `AC 40` header / product ID. LE u16 = `0x40AC` = 16556 — **same value as the advertised manufacturer ID**. Use as a packet-validity check. |
| 2 hi nibble | sign flag: `8` or `C` ⇒ negative |
| 2 lo nibble | stable/settled flag: `1` ⇒ still |
| 3 (lo nibble) .. 6 | 28-bit big-endian unsigned weight in **milligrams** |
| 7-17 | zero in all observed packets (unknown / reserved) |
| 18-19 | `A6 A7` trailer / checksum candidate — constant while weight constant |

### Authoritative decode (Bean Conqueror `blackcoffeeScale.ts`)

Bean Conqueror explicitly supports this scale. `DEVICE_NAME_SECOND = 'my_scale'`
and it uses exactly service `FFB0` / characteristic `FFB2`. Its parser:

```ts
public static DEVICE_NAME        = 'blackcoffee';
public static DEVICE_NAME_SECOND = 'my_scale';
public static DATA_SERVICE        = '0000ffb0-0000-1000-8000-00805f9b34fb';
public static DATA_CHARACTERISTIC = '0000ffb2-0000-1000-8000-00805f9b34fb';

private parseStatusUpdate(BlackcoffeeRawStatus: Uint8Array) {
  if (BlackcoffeeRawStatus.length > 14) {
    const hex = Array.from(new Uint8Array(BlackcoffeeRawStatus.buffer))
      .map((b) => b.toString(16).padStart(2, '0')).join('');
    const isNegative = hex[4] == '8' || hex[4] == 'c';
    const isStill    = hex[5] == '1';
    const hexWeight  = hex.slice(7, 14);
    // weight is in gram
    const weight = ((isNegative ? -1 : 1) * parseInt(hexWeight, 16)) / 1000;
    this.setWeight(weight);
  }
}
```
Source: https://raw.githubusercontent.com/graphefruit/Beanconqueror/master/src/classes/devices/blackcoffeeScale.ts

Note it indexes the **hex string by nibble**, not bytes. `hex.slice(7,14)` is 7
nibbles = 28 bits, starting mid-byte-3. Verified byte-wise equivalent for C++:

```c
uint32_t mg = ((uint32_t)(d[3] & 0x0F) << 24) |
              ((uint32_t)d[4] << 16) |
              ((uint32_t)d[5] << 8)  |
               (uint32_t)d[6];
bool neg    = ((d[2] >> 4) == 0x8) || ((d[2] >> 4) == 0xC);
bool stable = ((d[2] & 0x0F) == 0x1);
float grams = (neg ? -1.0f : 1.0f) * (float)mg / 1000.0f;
```
Confirmed to reproduce Bean Conqueror's integer exactly on the captured packet.

`/1000` means the field is **milligrams**. This is the one decode detail still
worth confirming against a known mass (see Open questions) — the zero packet
cannot distinguish scale factors.

## Taring — IMPORTANT

Bean Conqueror sets `supportsTaring = false` for this scale and logs
"Taring is not possible with the blackcoffee scale". `setTimer()` is likewise a
no-op ("doesnt feature a timer").

`FFB1` exists and is writable, but **no command bytes are documented anywhere**,
and Bean Conqueror never writes to it.

**Consequence for brew-by-weight: the firmware must implement SOFTWARE TARE** —
capture the weight at shot start as an offset and subtract it, rather than
commanding the scale to zero. This is strictly better anyway: it is instant,
cannot fail, and needs no round-trip. It also removes the "tare trap" concern,
though arming still must not happen mid-shot (the offset would be wrong).

## Deep sleep behaviour (VERIFIED)

The scale sleeps aggressively — it stopped advertising within ~2-3 min of being
left idle, repeatedly. Once asleep it is completely undiscoverable (direct
connect by address returns `BleakDeviceNotFoundError`). It must be physically
woken before a shot. Firmware therefore must:
- treat "no scale" as the normal case, never block a shot;
- scan continuously/periodically and connect opportunistically;
- arm BBW only if connected at the moment the shot starts.

## Open questions

1. ~~Scale factor confirmation.~~ **RESOLVED 2026-08-29 — CONFIRMED live.**
   See "Live confirmation" below.
2. **Byte 18-19 trailer** — algorithm UNKNOWN, and the obvious guess is WRONG.
   Not needed: we validate packets by the `AC 40` header and length instead.
3. **`FFB1` command bytes** beyond tare — unknown, and unnecessary.

## Live confirmation (2026-08-29) — ground truth

Captured from the real device with a calibrated load on the platform. These
three packets are now permanent regression vectors in
`test/test_myscale_parse.c` section 11.

| condition | packet | decoded |
|---|---|---|
| empty platform | `ac4001000000...a6a7` | `0.000 g`, stable |
| calibrated load | `ac4001000759fe...a605` | **`481.790 g`**, stable |
| load lifted after tare | `ac408100000564...a690` | `-1.380 g`, stable, sign nibble `0x8` |

**Scale factor `/1000` (milligrams) is CONFIRMED.** The loaded reading decodes
to 481.790 g — a plausible physical mass. Had the factor been `/100` it would
read 4817.9 g and `/10` would read 48179 g, both absurd for a kitchen scale, so
a single real load pins the factor unambiguously.

**Negative sign path is CONFIRMED live.** Previously only exercised against
synthetic vectors. Lifting the load after a tare produced genuine negative
readings, and the same object read `-481.83 g` versus `+481.790 g` — sign
symmetry within 40 mg, which also demonstrates decode repeatability.

Byte 18 was **constant `0xa6`** across all three packets while byte 19 varied
(`a7` / `05` / `90`), so byte 18 is not part of any checksum over the payload —
further reason not to add trailer validation.

### Trailer bytes: "sum checksum" is FALSIFIED (tested, do not implement)

GaggiMate's `myscale.cpp` defines `calculateChecksum()` as "sum of all bytes
except the last", but **never calls it** in the parse path. It is tempting to
assume the trailer is that checksum. Tested against the real captured packet
`ac4001...a6a7`:

| hypothesis | computed | actual | match |
|---|---|---|---|
| `byte[19] == sum(bytes[0..18]) % 256` | `0x93` | `0xA7` | **NO** |
| `byte[18] == sum(bytes[0..17]) % 256` | `0xED` | `0xA6` | **NO** |
| 16-bit sum of `bytes[0..17]` | `0x00ED` | `0xA6A7` | **NO** |
| XOR of `bytes[0..18]` | `0x4B` | `0xA7` | **NO** |

So the dormant `calculateChecksum()` does **not** describe this device's
trailer. `A6 A7` stayed constant across ~100 packets while weight was constant,
so a single distinct value cannot reveal the algorithm. **Do not add checksum
validation** — it would reject every real packet. Header + length is the correct
validity check, which is what the firmware implements.

### Manufacturer ID is the SAME value as the packet header

The advertised manufacturer ID is **16556 = `0x40AC`**, and packet bytes 0-1 are
`AC 40` = little-endian `0x40AC`. These are the same 16-bit value, so the header
doubles as a device/product signature — which is *why* it is a sound validity
check. (A secondary source described these as unrelated and wrote the hex as
`0x409C`; that is a typo — `0x409C` is 16540, not 16556.)

## Bottom line for the integration

The existing `Discreet_MQTT.ino` BLE client targets
`6E400001/6E400004/6E400003` (Nordic UART / WeighMyBru) and sends
`[0x03,0x0A,cmd,0x01,0x00]` tare commands. **Against this scale it would never
connect, and the tare would never work.** The client must be rewritten for
FFB0/FFB2 with software tare.
