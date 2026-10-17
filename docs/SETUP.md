# MultiSense Tracker — MQTT build, setup & bring-up

ESP32-S3-WROOM-1 **N16R8** · SIMCom **A7672S** 4G · **AS7341** · **VL53L5CX** ·
**INA226** (R010, 2S Li-ion) · **WS2812B** · buzzer → **HiveMQ Cloud** →
your backend **and** your dashboard.

---

## 1. What this replaces, and why

Your old firmware was SIM7070G + INA219 pushing **HTTP POST** to the FastAPI
server. Three things change:

| | Before | Now |
|---|---|---|
| Transport | HTTP POST per packet | MQTT, one persistent TLS session |
| Downlink | device **polls** `/api/command/next` | broker **pushes** to `…/cmd` instantly |
| Geofence | evaluated in the browser only | evaluated **on the device** |

The downlink is the real reason for MQTT here. On HTTP polling, "find my
device" waited for the next report cycle — up to 30 s of silence while you
stand there holding your phone. Over MQTT it beeps in under a second. And
because the fence now lives in the tracker's NVS, the buzzer fires when it
leaves the zone **even with the dashboard closed and the server asleep** —
which is exactly when a tracker matters.

```
  ESP32-S3 ──I2C──┬─ AS7341      ┌──────────────┐
                  ├─ VL53L5CX    │  HiveMQ      │
                  └─ INA226      │  Cloud       │
       │                         │  (free tier) │
       └──UART──> A7672S ──4G──> └──┬────────┬──┘
                                    │        │
                 TLS 8883 ──────────┘        └────── WSS 8884
                        │                                 │
             ┌──────────▼──────────┐            ┌─────────▼──────────┐
             │ backend (Render)    │            │ dashboard (Vercel) │
             │ paho-mqtt bridge    │            │ mqtt.js — LIVE     │
             │ SQLite history, CSV │◀──REST─────│ history, playback  │
             └─────────────────────┘            └────────────────────┘
```

Both subscribe. The **backend** owns history, CSV and playback; the
**dashboard** takes live packets straight off the broker so it stays alive
even while Render's free tier is cold-starting. The dashboard no longer opens
the backend WebSocket when MQTT is configured, so no packet is ever rendered
twice.

---

## 2. Wiring

Pins avoid GPIO 26–37 (octal flash/PSRAM on the N16R8 — **35/36/37 will
brick your boot if you use them**) and 19/20 (native USB).

| Signal | ESP32-S3 GPIO | Goes to | Note |
|---|---|---|---|
| I2C SDA | 8 | AS7341 + VL53L5CX + INA226 | one 4.7 kΩ pull-up to 3V3 per line, not three |
| I2C SCL | 9 | ″ | 400 kHz |
| UART TX | 17 | A7672S **RX** | crossed |
| UART RX | 18 | A7672S **TX** | crossed |
| PWRKEY | 16 | A7672S PWRKEY | low >1 s powers on |
| WS2812B DIN | 21 | LED module DIN | 330 Ω series resistor on the data line |
| Buzzer | 4 | buzzer module | |
| VL53L5CX LPn | 5 | LPn | set `-1` in config.h if tied to 3V3 |

I2C addresses don't collide: AS7341 `0x39`, VL53L5CX `0x29`, INA226 `0x40`.

**Two power warnings, both of which will bite you:**

1. The A7672S pulls **~2 A peaks** during LTE transmit. It must run from the
   battery/supply through its own bulk cap (**≥1000 µF low-ESR at the module's
   VBAT**), *never* from the ESP32 board's 3V3 regulator. The classic symptom
   of getting this wrong is a modem that answers `AT` fine and then resets the
   instant it tries to register.
2. **INA226 shunt placement.** The module measures across its shunt between
   `IN+` and `IN−`; put it **high-side in the pack's positive lead**, with the
   module's `VBUS` on the load side. Bus voltage input tolerates 36 V, so 2S
   (8.4 V) is comfortable.

The WS2812B wants 5 V but accepts 3.3 V logic at 5 V supply in most cases; if
the colours glitch, drive DIN through a level shifter or run the LED from 3V3.

---

## 3. Broker: HiveMQ Cloud (free)

**Free tier: 100 concurrent connections, 10 GB/month traffic, TLS included,
no credit card, no expiry.** There is no separate student plan — the free tier
*is* the offer, and it comfortably covers this project (see §11 for the
arithmetic). The [GitHub Student Developer Pack](https://education.github.com/pack)
is worth claiming separately for cloud credits if you later self-host a broker.

1. Sign up at **https://console.hivemq.cloud** → *Create free cluster* →
   **Serverless**. Pick the region nearest you (`eu-central-1` or
   `ap-south-1` if offered — latency to India is better).
2. On the cluster's **Overview** tab copy the **hostname**
   (`something.s1.eu.hivemq.cloud`) and note the ports: **8883** TCP/TLS (the
   modem) and **8884** WebSocket/TLS (the browser).
3. **Access Management** → create **three** credentials, not one:

   | Username | Password | Permissions | Used by |
   |---|---|---|---|
   | `tracker-device` | strong | pub `multisense/#`, sub `multisense/#` | the A7672S |
   | `backend-bridge` | strong | sub `multisense/#`, pub `multisense/+/cmd`, `multisense/+/cfg/#` | server.py |
   | `dashboard-readonly` | **treat as public** | sub `multisense/+/telemetry`, `+/event`, `+/status`; pub `multisense/+/cmd`, `+/cfg/#` | the browser |

   Separate credentials matter because the third one **ships to every visitor
   in your page source**. Anyone who opens DevTools on your Vercel site can
   read it. Scoped as above, the worst a stranger can do is watch your
   telemetry and make your buzzer beep — not impersonate the device or wipe
   your history. If the site must be genuinely private, set
   `enabled: false` in `mqtt-live.js` and let the backend WebSocket be the
   live source instead; then nothing is exposed.

### Topics

| Topic | Dir | QoS | Retained | Payload |
|---|---|---|---|---|
| `multisense/<DEV>/telemetry` | up | 1 | no | the full sensor packet |
| `multisense/<DEV>/status` | up | 1 | **yes** | `{"state":"online"}` / `offline` via last-will |
| `multisense/<DEV>/event` | up | 1 | no | `{"type":"geofence_exit",…}` |
| `multisense/<DEV>/cmd` | down | 1 | no | `{"cmd":"find","value":30}` |
| `multisense/<DEV>/cfg/geofence` | down | 1 | **yes** | `{"enabled":true,"lat":…,"lon":…,"radius_m":200}` |

The fence is **retained** on purpose: a tracker that boots six hours from now
still receives the current zone the moment it subscribes. `status` is retained
so the dashboard knows the device's state before the first packet arrives.

---

## 4. Upload the CA certificate (once, and only once)

HiveMQ Cloud serves a Let's Encrypt chain, so the modem needs **ISRG Root X1**
in its own flash. It persists across reboots *and* across ESP32 reflashes — do
this once per modem and forget it.

```bash
pip install pyserial
python3 tools/upload_ca.py --port /dev/ttyUSB0      # or --port COM5
```

`certs/isrgrootx1.pem` is included (1939 bytes, valid to 2035).

> **The gotcha that costs people a weekend.** The AT command lines end with
> `CRLF`, but the certificate body must be sent with **bare LF**. Upload it
> with Windows line endings and the modem happily stores a certificate it
> cannot parse — then every connect attempt returns `+CMQTTCONNECT: 0,34`
> with no hint as to why. `upload_ca.py` normalises the endings and declares
> the byte count *after* normalising. If you ever do it by hand, that is the
> detail to get right.

Verify: `AT+CCERTLIST` should list `isrgrootx1.pem`. The firmware checks this
at boot and refuses to continue with a clear serial message rather than
failing mysteriously later.

---

## 5. Firmware

**Libraries** (Arduino Library Manager):
`Adafruit AS7341` · `SparkFun VL53L5CX Arduino Library` · `INA226` by Rob
Tillaart · `Adafruit NeoPixel` · `ArduinoJson` (v7).

**Board settings** — these three are not optional:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| **PSRAM** | **OPI PSRAM** ← the offline buffer needs it |
| Partition Scheme | Default 16MB with spiffs |
| USB CDC On Boot | Enabled (so you get serial output) |

**Edit only `firmware/multisense_s3_a7672s/config.h`:**

```c
#define DEVICE_ID   "TRK-A7672S-01"
#define APN         "airtelgprs.com"   // Jio: "jionet"  BSNL: "bsnlnet"
#define MQTT_HOST   "xxxx.s1.eu.hivemq.cloud"
#define MQTT_USER   "tracker-device"
#define MQTT_PASS   "..."
```

Everything else in that file is a tuning knob with a working default.

### Why a hand-written AT driver instead of TinyGSM

TinyGSM has no support for the A76XX **native** MQTT stack (`AT+CMQTT*`).
Going through TinyGSM means PubSubClient over a raw TCP socket, which means
doing TLS on the ESP32 instead of in the modem: slower handshakes, ~40 kB more
RAM, and you lose the modem's persistent session across sleep. `a7672s.cpp`
drives the modem's own stack, which is the right tool for this job.

One non-obvious thing in there, worth knowing before you debug it: the AT
read loops call `indTick()` while they wait. A `CMQTTPUB` round trip can block
for a second or two, and without that call the buzzer and LED would visibly
stutter every time a packet goes out.

---

## 6. Backend (Render free tier)

Replace your `backend/` folder with this one — `server.py` is your original
with the MQTT bridge added, and every existing REST route still behaves
identically. HTTP ingest still works, so your old `curl` test is unaffected.

Render → your service → **Environment**:

```
MQTT_HOST = xxxx.s1.eu.hivemq.cloud
MQTT_PORT = 8883
MQTT_USER = backend-bridge
MQTT_PASS = ...
MQTT_BASE = multisense
```

Leave `MQTT_HOST` **unset** and the bridge stays dormant and the server
behaves exactly as it does today — a useful way to isolate problems.

Start command is unchanged:
`uvicorn server:app --host 0.0.0.0 --port $PORT`

### What changed in the code

`ingest_packet()` used to do the SQLite insert and the WebSocket fan-out
inline. Both the HTTP route and the MQTT bridge now call one shared
`store_packet()`, so there is a single ingest path that normalises
timestamps, trims the ring buffer and fans out — rather than two copies that
drift apart. `POST /api/command` now publishes over MQTT and falls back to
the old HTTP poll queue. One new route, `POST /api/geofence`, pushes the
fence as a retained message.

**Render's free tier sleeps after ~15 min idle and its disk resets on
redeploy**, so history there is not durable. That is precisely why the
dashboard also subscribes directly — live data keeps flowing while Render
wakes up. Add a persistent disk when you want real history.

---

## 7. Frontend (Vercel)

Replace your `frontend/` folder with this one, then edit the block at the top
of **`mqtt-live.js`**:

```js
host: 'xxxx.s1.eu.hivemq.cloud',
port: 8884,                      // WSS — 8883 is the TCP port, it will not work
user: 'dashboard-readonly',
pass: 'CHANGE_ME',
deviceId: 'TRK-A7672S-01'
```

Commit, and Vercel redeploys itself. `vercel.json` already points at
`frontend/`, so nothing else changes.

### What changed in `index.html` (6 edits, all small)

1. Loads `mqtt.min.js` + `mqtt-live.js` **before** the dashboard script.
2. New `startLive()` picks **one** live source — MQTT if configured, else the
   backend WebSocket. This is what stops double-rendering: the two paths
   stamp different arrival timestamps, so no downstream dedupe could have
   caught it.
3. `sendCmd()` publishes over MQTT first, falls back to `POST /api/command`.
4. All three places that mutate the geofence now also publish it to the
   device (the radius slider is debounced 600 ms so dragging it doesn't spam
   the broker).
5. Find-device now asks for **30 s** of beeping instead of 3 — long enough to
   actually walk around and locate it.
6. Default device id updated.

Every call into the MQTT layer is guarded (`window.gfPublish && …`), so if
`mqtt-live.js` is absent or misconfigured the dashboard degrades to exactly
its current behaviour. Your existing `npm test` harness passes unmodified.

---

## 8. LED and buzzer legend

Print this and tape it to the enclosure. Only the **highest-priority** active
condition is shown, top of the table wins.

| Condition | LED | Buzzer |
|---|---|---|
| Fatal error (sensor/modem init failed) | **red, non-stop 150 ms** | 2 low beeps every 1.5 s |
| **Outside the geofence** | magenta, 200 ms | **3 beeps @ 2.7 kHz every 2 s** |
| **Find device** (dashboard button) | white strobe, 60 ms | **warble @ 4 kHz, near-continuous** |
| Battery critical (<10 %) | red, 250 ms | chirp every 60 s |
| **Battery low (<20 %)** | **red, 1 s blink** | chirp every 60 s |
| Offline / no network | amber, 1 s blink | silent |
| Online, no GNSS fix | cyan, every 2 s | silent |
| Charging | green breathe | silent |
| Booting | white breathe | one chirp when ready |
| Healthy | green heartbeat every 3 s | silent |

Reading it in the field: **red = look at the battery or the serial log**,
**amber = it's alive but can't reach the network**, **cyan = it's online but
hasn't found the sky yet**, **green = fine**. Magenta and white only ever
appear with sound.

`{"cmd":"mute","value":true}` silences the buzzer without touching the LED —
useful on the bench. `{"cmd":"led_test"}` runs the find pattern for 3 s.

---

## 9. Bring-up order

Do it in this order. Each step proves one link, so when something fails you
know which link it was. Skipping ahead is how people end up debugging four
things at once.

**Step 1 — the website, with no hardware at all.**
```bash
pip install paho-mqtt
MQTT_HOST=xxxx.s1.eu.hivemq.cloud MQTT_USER=tracker-device MQTT_PASS=... \
    python3 tools/fake_device.py --interval 5
```
Open your dashboard, untick **DEMO DATA**. Packets should appear, the map
trail should walk in a circle, and the heatmap should show an object on the
left. Then draw a geofence and click find-device: `fake_device.py` prints the
downlink messages it receives. **Both directions now proven, before you touch
a soldering iron.**

**Step 2 — the modem, by hand.** Flash `tools/at_passthrough/`. Serial
Monitor at 115200, line ending **Both NL & CR**:
```
AT                      → OK                 (wiring is right)
AT+CPIN?                → +CPIN: READY       (SIM seated, no PIN)
AT+CSQ                  → +CSQ: 18,99        (>10 is usable)
AT+CGDCONT=1,"IP","airtelgprs.com"
AT+CGATT=1
AT+CGREG?               → +CGREG: 0,1        (1 or 5 = registered)
AT+CCERTLIST            → isrgrootx1.pem     (step 4 worked)
AT+CGNSSPWR=1           → +CGNSSPWR: READY!
AT+CGNSSINFO            → a fix, or empty commas near a window
```
Get a GNSS fix outdoors or at a window **before** blaming the firmware — the
first cold fix can take several minutes.

**Step 3 — the sensors.** Flash the real firmware. The serial log prints a
self-check and then names any sensor that didn't answer:
```
self-check: PASS   (soc@3.85V=50%, patiala->chd=57.3 km)
```
That line is the two bits of maths that fail *silently* if edited — the SoC
curve and the haversine. If it says FAIL, fix that before trusting any
reading. Then watch for `MQTT connected` and `published 1300 bytes`.

**Step 4 — the full loop.** Dashboard live, draw a fence around your desk,
then walk out of it. Buzzer + magenta LED, and an alert on the dashboard.
Pull the antenna to force an outage and confirm it buffers and replays.

---

## 10. Calibration — three knobs that need real measurements

Nothing on a bench reads correctly by default. These are the three that
matter, in order of how wrong they are out of the box:

**INA226 current.** If your module has a different shunt than R010, change
`SHUNT_OHMS`; wrong here and *everything* downstream — current, SoC, battery
health, charge estimates — is wrong by the same ratio. Check it against a
bench supply's own ammeter at ~500 mA. If the sign is inverted (discharge
shown as charging), set `CURRENT_SIGN` to `-1` rather than rewiring.

**Pack internal resistance** (`BATT_IR_OHMS`, default 0.120 Ω for 2S). A 1 A
load sags a 2S pack ~120 mV, which the voltage curve would otherwise read as
~15 % of charge that isn't missing. To measure it: note the resting voltage,
apply a known load, note the voltage again, then `R = ΔV / I`. Only a voltage
curve with IR compensation — no coulomb counting, see §13.

**AS7341 lux.** `lux = clear * 0.164` is a single scale factor matched to the
`ATIME`/`ASTEP`/gain in `initSensors()`, not a real lux model. Compare against
any phone light meter and scale that constant. Change the gain and this
constant must change with it.

---

## 11. Data budget

A real packet with the full 8×8 grid and per-zone status is **~1.3 kB**.

| Report interval | Device → broker | Broker traffic (2 subscribers) |
|---|---|---|
| 30 s (default) | ~110 MB/month | ~340 MB/month |
| 10 s | ~340 MB/month | ~1.0 GB/month |
| 60 s | ~55 MB/month | ~170 MB/month |

Against HiveMQ's 10 GB/month there is ample headroom even at 10 s, which is
why the firmware sends the **full grid in every packet** (`GRID_EVERY_N 1`).

**Your SIM plan is the tighter limit, not the broker.** If the data cost
bites, set `GRID_EVERY_N 10`: stats go out every packet and the full heatmap
every tenth, cutting payload ~10× while the dashboard's heatmap keeps
working.

---

## 12. Troubleshooting

`+CMQTTCONNECT: 0,<err>` — the error code tells you exactly which link broke:

| err | Meaning | Fix |
|---|---|---|
| **34** | certificate/CA rejected | **the CRLF mistake in §4** — re-upload with `upload_ca.py` |
| 33 | cert not set | `AT+CSSLCFG="cacert",0,"isrgrootx1.pem"` didn't take; check `AT+CCERTLIST` |
| 32 | TLS handshake failed | SNI off — needs `AT+CSSLCFG="enableSNI",0,1` (firmware does this) |
| 30 | bad username/password | wrong credential, or you used the dashboard's |
| 31 | not authorized | credential exists but lacks permission on that topic |
| 25 | DNS error | no PDP context; re-run `AT+CGATT=1` and check `AT+CGREG?` |
| **9** | network not opened | run `AT+NETOPEN` (firmware tries this, some builds need it) |
| 27 | protocol version | broker rejected MQTT 3.1.1 — not a HiveMQ issue |

Other symptoms:

| Symptom | Cause |
|---|---|
| `AT` works, modem resets on registration | power supply — see §2, you need the bulk cap |
| VL53L5CX "not found" at boot | it needs ~1.2 s after power-up; also check `LPn` is high |
| Dashboard shows 1970 timestamps | a packet reached the UI with `ts:0` — `mqtt-live.js` stamps it, so this means a path bypassed it |
| Packets appear twice | both live sources running; `startLive()` prevents it, so check edit #2 in §7 survived |
| Browser connects, no packets | port 8884 not 8883, and the path must be `/mqtt` |
| Buzzer silent but LED right | `indMute` left on, or `BUZZER_PASSIVE` mismatches your module |
| LED wrong colours | WS2812B wants 5 V; try a level shifter on DIN |

Set `AT_DEBUG 1` in `config.h` (it's the default) to see every byte to and
from the modem on USB serial. That log answers most questions faster than
reasoning about them.

---

## 13. What I deliberately left out

- **Coulomb counting.** Voltage curve + IR compensation only. 2S Li-ion has
  enough slope for ~±5 % once §10 is calibrated. Add integration of
  `current_ma × dt` if you need better than that — the INA226 already gives
  you the current.
- **Flash-backed offline buffer.** The 60-packet replay buffer lives in
  PSRAM, so a reboot loses it. You chose RAM; switch to LittleFS if you need
  to survive power loss.
- **Deep sleep.** Always-on, as you chose: ~120–200 mA average, dominated by
  the modem. Needed for sub-second find-device.
- **Multi-device support.** Topics and the backend are already wildcard-based
  (`multisense/+/telemetry`), so a second tracker just needs its own
  `DEVICE_ID` and credential. The dashboard shows one device at a time.
- **Per-sensor retry.** A sensor that fails at boot stays failed until
  reboot, and shows the fatal-error LED. Add a re-init in the loop if you see
  it happen in the field.

## 14. Files

```
firmware/multisense_s3_a7672s/
  multisense_s3_a7672s.ino   main loop, packet build, fence, boot self-check
  config.h                   EVERY tunable — the only file you must edit
  a7672s.h/.cpp              modem: LTE attach, TLS, MQTT AT stack, GNSS
  indicators.h/.cpp          WS2812B + buzzer state machines, non-blocking
backend/
  server.py                  your server + the MQTT bridge wired in
  mqtt_bridge.py             paho-mqtt subscriber and downlink publisher
  test_bridge.py             run it: python3 test_bridge.py
frontend/
  index.html                 your dashboard, 6 small edits (§7)
  mqtt-live.js               live-over-WSS, command + fence publishing
tools/
  upload_ca.py               one-time CA upload (run this before first flash)
  fake_device.py             test the whole chain with no hardware
  at_passthrough/            type AT commands by hand
certs/isrgrootx1.pem         ISRG Root X1, valid to 2035
```
