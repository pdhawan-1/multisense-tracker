# MultiSense Tracker — MQTT edition (v3)

ESP32-S3 N16R8 + SIMCom A7672S 4G + AS7341 + VL53L5CX + INA226 + WS2812B +
buzzer, publishing over MQTT to HiveMQ Cloud, consumed by both your FastAPI
backend and your Vercel dashboard.

**Read `docs/SETUP.md`** — broker setup, the as-built wiring table, the
LED/buzzer legend, bring-up order and troubleshooting. Start at §9; it
proves the website works before you touch hardware.

## v3 changes at a glance

Pin map matches your physical wiring (I2C 8/9 · ToF INT 4 RST 5 · AS7341
INT 6 · INA226 ALE 15 · buzzer 2 · WS2812 1 · modem TX 43 RX 44 · no
PWRKEY). **GPIO 7 is unused** — the INA226's VBS belongs on the pack, not a
GPIO. And `USB CDC On Boot` must be **Enabled**, because 43/44 are the ROM
debug UART.

New: filtered coulomb-counting fuel gauge · adaptive reporting · emergency
and last-gasp battery modes · trips + odometer · 4-zone geofence · accident
detection · network time · watchdog · cell-tower fallback · SMS fallback and
commands · modem temperature · all three INT pins + I2C auto-recovery.
Details and caveats in SETUP.md §10b–10c.

## Copy into your repo

| From this bundle | To your repo |
|---|---|
| `firmware/multisense_s3_a7672s/` | `firmware/multisense_s3_a7672s/` (keep the old sketch if you like) |
| `backend/server.py`, `mqtt_bridge.py`, `requirements.txt`, `test_bridge.py` | `backend/` |
| `frontend/index.html`, `mqtt-live.js` | `frontend/` |
| `test/v3-extras.mjs` | `test/` (new; `harness.mjs` is unchanged) |
| `tools/`, `certs/`, `docs/`, `package.json` | repo root |

`demo-data.js`, `vercel.json` and `test/harness.mjs` are byte-identical to
yours. `package.json` only gains the `test:v3` / `test:all` scripts.

## Three edits before anything works

1. `firmware/multisense_s3_a7672s/config.h` — `APN`, `MQTT_HOST`, `MQTT_USER`, `MQTT_PASS`
2. `frontend/mqtt-live.js` — the `MQ` block at the top (port **8884**, not 8883)
3. Render environment — `MQTT_HOST`, `MQTT_USER`, `MQTT_PASS`

## Checks you can run now

```bash
python3 backend/test_bridge.py    # topics, timestamps, command + zone payloads
npm install && npm run test:all   # original harness + 25 v3 dashboard checks
MQTT_HOST=… MQTT_USER=… MQTT_PASS=… python3 tools/fake_device.py
```

All three pass on this bundle, and every firmware file compiles clean under
g++ with the library APIs stubbed (0 warnings).
