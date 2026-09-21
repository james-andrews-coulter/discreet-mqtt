# Discreet → Home Assistant (MQTT + brew-by-weight)

Replaces the Discreet web UI with a Home Assistant dashboard and adds auto-stop by weight using a `MY_SCALE` Bluetooth scale. Assumes your Discreet machine is already built and working.

---

## Give this repo to your AI

Copy-paste this prompt:

> Set up the integration in https://github.com/james-andrews-coulter/discreet-mqtt. Follow its README step by step. You have the repo locally — read the files you need, don't ask me to copy-paste YAML.

---

## You need

- Discreet machine with ESP32 already installed
- Home Assistant with **Mosquitto broker** add-on + **MQTT** integration enabled
- `MY_SCALE` Bluetooth scale (the `MY_SCALE` model only — not WeighMyBru/Bean Conqueror)

## Setup

**1. Flash firmware**

Toolchain: ESP32 Arduino core **2.0.17** + NimBLE **1.4.3** (other versions will not fit / not compile).

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 Discreet_MQTT --output-dir build
# first time: USB
arduino-cli upload -p /dev/cu.usbserial-* --fqbn esp32:esp32:esp32 Discreet_MQTT
# later updates: OTA (machine must be on and idle)
./flash-discreet.sh
```

**2. SD card config**

Copy `config.json.example` → `config.json` on the SD card root. Fill in WiFi + MQTT. Add:

```json
"target_weight": 36.0,
"bbw_enabled": true,
"cut_on_scale_loss": false
```

**3. Home Assistant package**

```yaml
# configuration.yaml
homeassistant:
  packages: !include_dir_named packages
```
Copy `packages/discreet.yaml` → `<ha-config>/packages/discreet.yaml` and restart HA.

**4. Dashboard**

HA → Settings → Dashboards → Add Dashboard → ⋮ → Raw configuration editor → paste `dashboard.yaml` → Save.

## Check it works

- Serial monitor @115200 shows `Scale connected (MY_SCALE FFB0)` after waking the scale
- HA → **Tare Scale** → weight goes to ~0 g
- Put 500 g on the scale → HA reads 500.0 g
- Wake scale, cup on, pull shot → auto-cuts at target

> The scale sleeps after ~2 min. Press its button before every shot.

## If something's wrong

| Problem | Fix |
|---|---|
| Entities missing | Check `packages:` line in `configuration.yaml`, restart HA |
| Scale never connects | Wake scale, confirm it's `MY_SCALE`, move other BLE devices away |
| Weight ~0 with cup on | Press **Tare Scale** |

Details: `docs/my-scale-ble-protocol.md` · Wiring: `docs/upstream/` · Issues: [GitHub Issues](https://github.com/james-andrews-coulter/discreet-mqtt/issues)

## License

GPLv3 — derivative of https://github.com/Discreet-Coffee/Discreet (GPLv3). Scale decode cross-checked against GaggiMate (MIT, `docs/upstream/`).
