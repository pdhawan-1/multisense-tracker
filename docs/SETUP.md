# MultiSense Tracker — MQTT build, setup & bring-up (v3)

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

## 2. Wiring (as built)

Pins avoid GPIO 26–37 (octal flash/PSRAM on the N16R8 — **35/36/37 will
break boot**), GPIO 19/20 (native USB) and the strapping pins 0/3/45/46.

| Signal | GPIO | Goes to | Note |
|---|---|---|---|
| I2C SDA | **8** | AS7341 + VL53L5CX + INA226 | one 4.7 kΩ pull-up per line, not three |
| I2C SCL | **9** | ″ | 400 kHz |
| VL53L5CX INT | **4** | INT | data-ready, active low |
| VL53L5CX RST | **5** | RST | hardware recovery — see the polarity note below |
| AS7341 INT | **6** | INT | light-threshold crossing |
| INA226 ALE | **15** | ALERT | bus under-voltage (~6.2 V on 2S) |
| Buzzer | **2** | buzzer +ve | |
| WS2812B | **1** | DIN | 330 Ω series resistor on the data line |
| UART TX | **43** | A7672S **RX** | crossed |
| UART RX | **44** | A7672S **TX** | crossed |
| PWRKEY | — | not wired | the module self-starts on power |

I2C addresses don't collide: AS7341 `0x39`, VL53L5CX `0x29`, INA226 `0x40`.

### GPIO 43/44 — the one setting you must not miss

Those are **U0TXD/U0RXD**, the ROM's default debug UART. They are free for
the modem only because `Serial` goes over native USB:

> **Tools → USB CDC On Boot = Enabled**

Get this wrong and two things happen: you see no serial output, and the ROM
bootloader's startup banner is transmitted straight into the modem's RX on
every reset. The modem ignores it, but you will be debugging a phantom.

If your board instead has a USB-UART bridge chip wired to 43/44, the bridge
and the modem fight over those pins — move the modem to GPIO 17/18 in
`config.h` and nothing else changes.

### GPIO 7 is deliberately unused

The INA226's **VBS** pin is the chip's own bus-voltage sense input; it
belongs on the 2S pack node, and the voltage reaches us over I2C. If a wire
still runs from VBS to GPIO 7, **remove it** — that pin is 3.3 V maximum and
8.4 V will damage it.

### VL53L5CX RST polarity

The sensor's own pin is `I2C_RST`: a HIGH pulse resets its I2C interface and
it idles LOW, which is what `TOF_RST_ACTIVE_HIGH 1` does. Some breakouts
instead bring out an active-LOW *enable*, where idling LOW holds the sensor
off and it never answers. If the ToF reports "not found" with correct
wiring, set `TOF_RST_ACTIVE_HIGH 0` and try again.

### Two power warnings, both of which will bite you

1. The A7672S pulls **~2 A peaks** on LTE transmit. It must run from the
   battery through its own bulk cap (**≥1000 µF low-ESR at the module's
   VBAT**), never from the ESP32 board's 3V3 regulator. The classic symptom
   is a modem that answers `AT` fine and then resets the instant it
   registers.
2. **INA226 shunt placement:** high-side in the pack's positive lead,
   between `IN+` and `IN−`, with `VBS` on the load side. Bus voltage input
   tolerates 36 V, so 2S (8.4 V) is comfortable.

## 3. Broker: EMQX free public broker

The firmware and dashboard now point at **`broker.emqx.io`** — EMQX's free
public broker. No signup, no account, no password, **and no CA certificate
to upload**, which removes the single most painful step of the old setup.

| | Device (A7672S) | Dashboard (browser) |
|---|---|---|
| Host | `broker.emqx.io` | `broker.emqx.io` |
| Port | **1883** (plain) | **8084** (WSS) |
| Path | — | `/mqtt` |
| Auth | none | none |

**Use port 8084, not 8083, in the browser.** Your dashboard is served over
HTTPS from Vercel, and an HTTPS page cannot open a plain `ws://` socket —
Chrome blocks it as mixed content and the only clue is a console error.

### Understand what "public" means

Every message you publish is readable by anyone, and anyone who learns your
topic can publish commands to your device — beep the buzzer, change the
report interval, move the geofence. Two mitigations are wired in:

1. **A random topic root.** Instead of `multisense/...` everything lives
   under `ms-89a4f93827c1/...`. Nobody browsing the broker will stumble
   onto it.
2. **A shared secret on every downlink.** The firmware drops any command or
   geofence push whose JSON lacks `"k": "RCxM5n3tiwlYoPDZlbMrnhrS"`.

**Neither is security.** Both strings are in your page source, visible to
anyone who opens DevTools on your Vercel site, and anyone who captures one
of your own messages has the root. They stop casual interference while you
bring the device up; they do not protect a tracker carrying real location
data. Treat this as the testing configuration.

These three values must match in **four** places or nothing talks:

| Value | Firmware `config.h` | Dashboard `mqtt-live.js` | Backend env |
|---|---|---|---|
| topic root | `TOPIC_ROOT` | `base` | `MQTT_BASE` |
| secret | `CMD_SECRET` | `secret` | `MQTT_SECRET` |
| device id | `DEVICE_ID` | `deviceId` | — (wildcard) |

Regenerate the root and secret any time you like:

```bash
python3 -c "import secrets; print('root  ', 'ms-'+secrets.token_hex(6)); print('secret', secrets.token_urlsafe(18))"
```

### Topics

| Topic | Dir | QoS | Retained | Payload |
|---|---|---|---|---|
| `<root>/<DEV>/telemetry` | up | 1 | no | the full sensor packet |
| `<root>/<DEV>/status` | up | 1 | **yes** | `online` / `offline` via last-will |
| `<root>/<DEV>/event` | up | 1 | no | `{"type":"accident",…}` |
| `<root>/<DEV>/cmd` | down | 1 | no | `{"cmd":"find","value":30,"k":"…"}` |
| `<root>/<DEV>/cfg/geofence` | down | 1 | **yes** | zones array + `"k"` |

The fence is **retained** on purpose: a tracker that boots six hours from
now still gets the current zones the moment it subscribes.

### When you outgrow it

Moving to a real broker is a config change, not a code change — the TLS and
credential paths are already written and compile-tested.

**Option A — EMQX Serverless (free tier, hosted).** Sign up, create a
deployment, add a username/password, then in `config.h` set
`MQTT_USE_TLS 1`, `MQTT_PORT 8883`, your host and credentials; upload the
CA once with `tools/upload_ca.py` (§4); in `mqtt-live.js` set the host,
port 8084, and the credentials; set `MQTT_TLS=1` plus `MQTT_USER`/`MQTT_PASS`
in the backend env. Free monthly quota, 1,000 connections, then
pay-as-you-go.

**Option B — your own Mosquitto on an Oracle Cloud Always Free VM.** Free
forever with no quota that can change under you, and you own it. Keep
`MQTT_USE_TLS 0` with a username/password and a firewall rule, or add
Let's Encrypt TLS later. The trade is that you administer a small Linux box.

## 4. CA certificate — not needed right now

`MQTT_USE_TLS` is **0**, so the modem connects in the clear on port 1883 and
there is no certificate to install. Skip straight to §5.

You need this section only when you switch to a TLS broker. Then:

```bash
pip install pyserial
python3 tools/upload_ca.py --port /dev/ttyUSB0      # or --port COM5
```

`certs/isrgrootx1.pem` (ISRG Root X1, valid to 2035) is included and covers
any broker serving a Let's Encrypt chain. The certificate lives in the
modem's own flash and survives reboots and ESP32 reflashes — one upload per
modem, forever.

> **The gotcha that costs people a weekend.** AT command lines end with
> `CRLF`, but the certificate body must be sent with **bare LF**. Upload it
> with Windows line endings and the modem stores a certificate it cannot
> parse, then every connect returns `+CMQTTCONNECT: 0,34` with no hint why.
> `upload_ca.py` normalises the endings and declares the byte count after
> normalising.

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

**Edit only `firmware/multisense_s3_a7672s/config.h`.** For the public
broker the one line you must change is your APN:

```c
#define APN         "airtelgprs.com"   // Jio: "jionet"  BSNL: "bsnlnet"
```

The broker block is already set up for `broker.emqx.io` on plain 1883 with
no credentials and no certificate. Everything else in that file is a tuning
knob with a working default.

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
MQTT_HOST   = broker.emqx.io
MQTT_PORT   = 1883
MQTT_TLS    = 0
MQTT_BASE   = ms-89a4f93827c1
MQTT_SECRET = RCxM5n3tiwlYoPDZlbMrnhrS
```

`MQTT_USER`/`MQTT_PASS` stay unset on the public broker. `MQTT_BASE` and
`MQTT_SECRET` **must** match `TOPIC_ROOT` and `CMD_SECRET` in `config.h`.

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

Replace your `frontend/` folder with this one. The block at the top of
**`mqtt-live.js`** is already set for the public broker — only touch it when
you change brokers or regenerate the root/secret:

```js
host: 'xxxx.s1.eu.hivemq.cloud',
host:   'broker.emqx.io',
port:   8084,                    // WSS. 8083 is plain ws and HTTPS blocks it
base:   'ms-89a4f93827c1',       // = TOPIC_ROOT in config.h
secret: 'RCxM5n3tiwlYoPDZlbMrnhrS',  // = CMD_SECRET in config.h
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
| **Accident suspected** | **hot red strobe, 90 ms** | **4 beeps @ 3.5 kHz, urgent** |
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
python3 tools/fake_device.py --interval 5
```
No environment variables needed — it defaults to the same public broker and
topic root as the firmware.
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
AT+CGNSSPWR=1           → +CGNSSPWR: READY!
AT+CGNSSINFO            → a fix, or empty commas near a window
```
Get a GNSS fix outdoors or at a window **before** blaming the firmware — the
first cold fix can take several minutes.

**Step 3 — the sensors.** Flash the real firmware. The serial log prints two
self-checks and then names any sensor that didn't answer:
```
battery self-check: PASS  (soc@3.85V/cell=50%, median rejects spike=7.40, 1A/30min=2000 mAh left)
tracker self-check: PASS  (patiala->chd=58.0 km, red-light=0.23g ignored, hard-stop=1.13g fires, threshold=0.60g)
watchdog armed at 90 s
```
Those cover the maths that fails *silently* when edited: the SoC curve, the
despike filter, the coulomb integrator, the haversine, and the crash
discriminator. If either says FAIL, fix it before trusting any reading.
Then watch for `MQTT connected` and `published ~1600 bytes`.

**Step 4 — the full loop.** Dashboard live, draw a fence around your desk,
then walk out of it. Buzzer + magenta LED, and an alert on the dashboard.
Pull the antenna to force an outage and confirm it buffers and replays.

---

## 10. Calibration — three knobs that need real measurements

Nothing on a bench reads correctly by default. These are the three that
matter, in order of how wrong they are out of the box:

**INA226 current.** If your module has a different shunt than R010, change
`SHUNT_OHMS`; wrong here and *everything* downstream — current, SoC, the
coulomb counter, runtime estimates — is wrong by the same ratio. Check it
against a bench supply's own ammeter at ~500 mA. If the sign is inverted
(discharge shown as charging), set `CURRENT_SIGN` to `-1` rather than
rewiring.

**Pack capacity** (`BATT_CAPACITY_MAH`, 2500 for your 2S1P LiPo). This is
what the coulomb counter integrates against, so a pack that is really
2000 mAh will read 25 % optimistic all the way down. Measure it once: charge
full, run until the device cuts out, and read the mAh the gauge reported.

**Pack internal resistance** (`BATT_IR_OHMS`, default 0.120 Ω for 2S). A 1 A
load sags a 2S pack ~120 mV, which the voltage curve alone would read as
~15 % of charge that isn't missing. Measure it: note the resting voltage,
apply a known load, note it again, `R = ΔV / I`.

**AS7341 lux.** `lux = clear * 0.164` is a single scale factor matched to the
`ATIME`/`ASTEP`/gain in `initSensors()`, not a real lux model. Compare against
any phone light meter and scale that constant. Change the gain and this
constant must change with it.

---

## 10b. How the fuel gauge actually works

Four stages, in `battery.cpp`:

1. **median-of-5** on voltage and current — kills single-sample spikes
2. **EMA** (`FILT_EMA_ALPHA 0.20`) — smooths what survives
3. **coulomb counting** against `BATT_CAPACITY_MAH`
4. **OCV fusion** — nudges the integral back toward the voltage curve, but
   **only while the pack is resting** (`|I| < 60 mA` for 20 s)

Why both: coulomb counting drifts, because every mA of offset error
accumulates forever. The voltage curve is noisy under load — the modem's
2 A bursts sag a 2S pack ~240 mV, which a voltage-only gauge reports as a
flat battery. Each method fixes the other's failure mode, which is why both
are in there.

The packet carries the evidence, not just the answer: `soc_pct` (fused),
`soc_ocv` (what the curve alone says), `mah`, `resting` and `soc_src`. If
those two percentages drift apart over days, your `SHUNT_OHMS` or
`BATT_CAPACITY_MAH` is off — that comparison is the diagnostic.

On boot the stored coulomb state is compared against the OCV reading; if
they disagree by more than 20 % the pack was charged or swapped while the
device was off, so OCV wins and the stored value is discarded.

## 10c. What v3 added, and what each thing costs you

| Feature | What it does | Honest caveat |
|---|---|---|
| **Adaptive reporting** | 10 s moving · 60 s idle · 5 min parked · instant on any event | `set_interval` from the dashboard overrides it until reboot |
| **Emergency mode** | ≤10 % SoC: position-only packets every 15 min, ToF and spectral off, LED dimmed to ⅓ | Exits at 15 % (hysteresis), so it won't flap |
| **Last gasp** | ≤3 % or ≤6.20 V: one final position + event + SMS, then stops publishing | Fires once per power cycle |
| **Trips + odometer** | Auto start/stop, distance, duration, max speed; lifetime km in NVS | Distance gates out steps <5 m, so a slow crawl under-counts slightly |
| **4-zone geofence** | Up to 4 named zones, retained on the broker, cached in NVS | The alarm means **outside every armed zone** — inside any one counts as allowed |
| **Accident detection** | Harsh decel → sustained standstill → alert with coordinates and the numbers behind it | **See the warning below** |
| **Network time** | `AT+CCLK` gives real timestamps, so buffered packets carry true times | Needs the network to supply time; falls back to arrival-stamping |
| **Watchdog** | 90 s TWDT; a wedged modem reboots the device | Reset cause published on boot as `diag.reset_reason` |
| **Cell-tower fallback** | Position from the serving cell after 3 min with no GNSS | Hundreds of metres to kilometres. Flagged `gnss.src: "lbs"` with `acc_m`, and the dashboard labels it CELL TOWER — never treat it as a trail point |
| **SMS fallback** | Texts a position after 15 min offline; accepts FIND / WHERE / STATUS / MUTE / REBOOT | Only from `SMS_OWNER` unless you set `SMS_ALLOW_ANY_SENDER`. Needs SMS on your plan |
| **Modem temperature** | `AT+CPMUTEMP`, free extra telemetry | **Die** temperature — reads 10–20 °C above ambient. Use it for over-temp warnings, not weather |
| **INT pins + I2C recovery** | ToF data-ready, AS7341 threshold, INA226 under-voltage; a wedged bus is clocked free and sensors re-inited | Recovery count published as `diag.i2c_recoveries` — if it climbs, fix your pull-ups |

### Accident detection — read this before you trust it

The discriminator is **deceleration in g**, not a km/h drop, because a
red-light stop and a crash look identical by drop alone. Measured by the
build's own self-check:

```
  gentle coast 40->0 over 8 s    0.14 g      ignored
  red light    40->0 over 5 s    0.23 g      ignored
  brisk stop   50->0 over 4 s    0.35 g      ignored
  hard brake   60->0 over 3 s    0.57 g      ignored
  IMPACT       40->0 over 1 s    1.13 g   -> ALERT
  IMPACT       80->5 over 1 s    2.12 g   -> ALERT
```

Threshold is `CRASH_DECEL_G 0.60`. Anything looser fires at every traffic
light. An alert also requires a prior speed ≥25 km/h and a standstill held
for 15 s, and it self-cancels with an `accident_cleared` event if the
vehicle drives off within 60 s.

**The honest limit:** this is 1 Hz GNSS speed, not an accelerometer. It
cannot see a low-speed knock, a rollover, or an impact while already
stopped, and it will occasionally fire on a hard stop in traffic. If
accident detection actually matters to you, add an **MPU6050 or LSM6DSO on
the same I2C bus** — then the accelerometer becomes the trigger and this
logic becomes the corroboration. That is a genuinely small addition; ask and
I'll wire it in.

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
| **34** | certificate/CA rejected | **the CRLF mistake in §4** — re-upload with `upload_ca.py`. Not possible with `MQTT_USE_TLS 0` |
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
| Device connects then drops every few seconds | **duplicate client id on the public broker.** The firmware appends the MAC tail to make it unique — if you hardcoded one, undo that |
| Dashboard never connects, console says mixed content | you used port 8083 (`ws://`). HTTPS pages need **8084** (`wss://`) |
| Commands do nothing, serial says "wrong or missing command secret" | `CMD_SECRET`, `secret` and `MQTT_SECRET` disagree across the three files |
| Telemetry arrives but the dashboard shows nothing | topic root mismatch — `TOPIC_ROOT` vs `base` vs `MQTT_BASE` |
| Someone else's data appears in your dashboard | you are on the public broker with a guessable root. Regenerate it |
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

- **Light sleep.** Not selected. Always-on draws ~120–200 mA, dominated by
  the modem, so a 2500 mAh pack gives roughly 15–17 h. Adaptive reporting
  already cuts the radio duty cycle hard; light sleep between sensor reads
  would save perhaps another 30–40 mA.
- **An accelerometer.** The single biggest upgrade available to this board —
  see the accident-detection warning above.
- **Tamper detection, silent mode, speed/stationary alerts, OTA, in-pocket
  and cargo detection.** Not selected. The AS7341 INT hook that tamper
  detection would use is already wired and publishing `light_change`
  events, so that one is a short step if you want it.
- **Flash-backed offline buffer.** The 60-packet replay buffer lives in
  PSRAM, so a reboot loses it. You chose RAM; switch to LittleFS if you need
  to survive power loss.
- **Per-sensor retry on a schedule.** Sensors are re-inited on I2C recovery,
  but a sensor that is simply absent at boot stays absent until reboot.
- **Multi-device support.** Topics and the backend are already wildcard-based
  (`multisense/+/telemetry`), so a second tracker just needs its own
  `DEVICE_ID` and credential. The dashboard shows one device at a time.
- **Per-sensor retry.** A sensor that fails at boot stays failed until
  reboot, and shows the fatal-error LED. Add a re-init in the loop if you see
  it happen in the field.

## 14. Files

```
firmware/multisense_s3_a7672s/
  multisense_s3_a7672s.ino   orchestration, packet build, SMS, modes
  config.h                   EVERY tunable — the only file you must edit
  a7672s.h/.cpp              modem: LTE, TLS, MQTT AT stack, GNSS, time,
                             temperature, cell-tower position, SMS
  battery.h/.cpp             filtered coulomb-counting fuel gauge
  sensors.h/.cpp             AS7341 + VL53L5CX, INT pins, I2C recovery
  tracker.h/.cpp             trips, odometer, 4-zone fence, accident, rate
  indicators.h/.cpp          WS2812B + buzzer state machines, non-blocking
backend/
  server.py                  your server + the MQTT bridge wired in
  mqtt_bridge.py             paho-mqtt subscriber and downlink publisher
  test_bridge.py             run it: python3 test_bridge.py
frontend/
  index.html                 your dashboard + the v3 extras strip and
                             multi-zone geofence UI
  mqtt-live.js               live-over-WSS, command + multi-zone fence
test/
  harness.mjs                your original dashboard smoke test (unchanged)
  v3-extras.mjs              25 checks on the v3 fields and zone publishing
tools/
  upload_ca.py               one-time CA upload (run this before first flash)
  fake_device.py             test the whole chain with no hardware
  at_passthrough/            type AT commands by hand
certs/isrgrootx1.pem         ISRG Root X1, valid to 2035
```
