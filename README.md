# MultiSense Tracker — MQTT edition

ESP32-S3 N16R8 + SIMCom A7672S 4G + AS7341 + VL53L5CX + INA226 + WS2812B +
buzzer, publishing over MQTT to HiveMQ Cloud, consumed by both your FastAPI
backend and your Vercel dashboard.

**Read `docs/SETUP.md`** — it has the broker setup, wiring, the LED/buzzer
legend, the bring-up order and the troubleshooting table. Start at §9
(bring-up order); it proves the website works before you touch hardware.

## Copy into your repo

| From this bundle | To your repo |
|---|---|
| `firmware/multisense_s3_a7672s/` | `firmware/multisense_s3_a7672s/` (keep the old sketch if you like) |
| `backend/server.py`, `mqtt_bridge.py`, `requirements.txt`, `test_bridge.py` | `backend/` (overwrites `server.py` + `requirements.txt`) |
| `frontend/index.html`, `mqtt-live.js` | `frontend/` (overwrites `index.html`) |
| `tools/`, `certs/`, `docs/` | repo root |

`demo-data.js`, `vercel.json`, `package.json` and `test/` are unchanged —
they're included only so `npm test` runs from this folder as-is.

## Three edits before anything works

1. `firmware/multisense_s3_a7672s/config.h` — `APN`, `MQTT_HOST`, `MQTT_USER`, `MQTT_PASS`
2. `frontend/mqtt-live.js` — the `MQ` block at the top (port **8884**, not 8883)
3. Render environment — `MQTT_HOST`, `MQTT_USER`, `MQTT_PASS`

## Checks you can run now

```bash
python3 backend/test_bridge.py    # bridge logic: topics, timestamps, payloads
npm install && npm test           # the dashboard still boots and renders
MQTT_HOST=… MQTT_USER=… MQTT_PASS=… python3 tools/fake_device.py
```
