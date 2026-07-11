# MultiSense Tracker Console

Full stack for the indoor/outdoor tracking device:
AS7341 spectral + VL53L5CX multizone ToF (8x8 depth map) + INA219 power monitor on I2C,
SIM7070G (GNSS + LTE-M/NB-IoT) on UART.

## Layout
```
frontend/   index.html      — the dashboard (self-contained UI)
            demo-data.js    — simulated packets for the DEMO DATA switch
backend/    server.py       — FastAPI: ingest + REST + WebSocket + hosts frontend
            requirements.txt
            sample-packet.json
firmware/   esp32_sim7070g_tracker.ino — device firmware skeleton
```

## Run the backend
```bash
cd backend
pip install -r requirements.txt
uvicorn server:app --host 0.0.0.0 --port 8000
```
Open http://localhost:8000 — the dashboard loads in DEMO mode.
Untick **DEMO DATA** to go live (WebSocket push + REST fallback,
history backfilled from the server).

## Test without hardware
```bash
curl -X POST http://localhost:8000/api/telemetry \
     -H "Content-Type: application/json" \
     -d @backend/sample-packet.json
```
Send a few of these (tweak values) with DEMO off and watch the
dashboard update in real time.

## API
| Method | Path                      | Purpose                                |
|--------|---------------------------|----------------------------------------|
| POST   | /api/telemetry            | device pushes one JSON packet          |
| GET    | /api/telemetry/latest     | newest packet                          |
| GET    | /api/telemetry/history    | ?limit=120 — oldest→newest for charts  |
| GET    | /api/devices              | device ids, last seen, packet counts   |
| DELETE | /api/telemetry            | clear stored packets                   |
| POST   | /api/command              | queue a device command (set_interval / buzzer / gnss_restart) |
| GET    | /api/command/next         | device polls + drains one command      |
| GET    | /api/command/pending      | inspect the queue                      |
| GET    | /api/telemetry/export.csv | ?limit=1000 — flattened CSV download   |
| WS     | /ws                       | every new packet pushed as JSON        |

Auth (optional): `export TRACKER_API_KEY=yoursecret` before starting the
server, then send header `X-API-Key: yoursecret` from the device
(already wired in the firmware sketch via `API_KEY`).

Storage: SQLite ring buffer (default 50 000 packets,
`TRACKER_MAX_ROWS` to change) in `backend/telemetry.db`.

## Device firmware
Edit the config block at the top of
`firmware/esp32_sim7070g_tracker.ino` (APN, SERVER_HOST, API_KEY),
install the listed libraries, flash. Notes:
- `lux` uses a rough scale factor — calibrate against a reference.
- VL53L5CX: 8x8 @ 15 Hz max; the 64-zone grid + status arrays are sent
  in every packet (payload ~1.5 kB — BODYLEN already raised to 2048).
- Dashboard insights derived on-device-data: depth heatmap, nearest-object
  alert, valid-zone confidence, CCT + light-source guess (AS7341),
  session trip odometer (GNSS).
- INA219 sign convention: current positive while charging.
- If Cat-M1 coverage is weak, try `AT+CNMP=2` (auto) and `AT+CMNB=3`.


## Deploying the website (no prior knowledge needed)

The repo is pre-configured: `vercel.json` at the root tells Vercel to
serve the `frontend/` folder, so there is **nothing to configure**.

### Step 1 — Put the code on GitHub (no software to install)
1. Go to https://github.com and click **Sign up** (or Sign in).
2. Click the **+** at the top-right → **New repository**.
3. Repository name: `multisense-tracker` → keep **Public** → click
   **Create repository** (do NOT tick any "initialize" boxes).
4. On the next page click the link **"uploading an existing file"**.
5. Open the extracted project folder on your computer, select ALL the
   files and folders inside it (frontend, backend, firmware, test,
   README.md, vercel.json, .gitignore, package.json) and **drag them
   into the browser window**. Chrome keeps the folder structure.
6. Click the green **Commit changes** button. Done — your code is on
   GitHub.

### Step 2 — Deploy the dashboard on Vercel (free)
1. Go to https://vercel.com and click **Sign Up** → **Continue with
   GitHub** (this links the two accounts).
2. Click **Add New…** → **Project**.
3. You'll see your GitHub repos — click **Import** next to
   `multisense-tracker`.
4. Change nothing on the configure screen (vercel.json handles it) →
   click **Deploy**.
5. ~30 seconds later you get a URL like
   `https://multisense-tracker.vercel.app` — that's your live site.
   Demo mode works immediately; share the link with anyone.

From now on, ANY change you upload to GitHub redeploys automatically.

### Step 3 (optional) — Deploy the backend for real device data
Vercel can't run this backend (it needs WebSockets + a persistent
process), so use Render's free tier:
1. Go to https://render.com → **Sign in with GitHub**.
2. **New** → **Web Service** → pick the `multisense-tracker` repo.
3. Set **Root Directory** = `backend`,
   **Build Command** = `pip install -r requirements.txt`,
   **Start Command** = `uvicorn server:app --host 0.0.0.0 --port $PORT`,
   Instance type = **Free** → **Deploy**.
4. You get `https://<your-app>.onrender.com` — this URL serves the FULL
   stack (dashboard + API + WebSocket), because server.py hosts the
   frontend too. Point the ESP32's `SERVER_HOST` at this domain.
5. To make the *Vercel* copy talk to it as well, edit the CONFIG block
   near the top of `frontend/index.html`:
   `apiUrl: 'https://<your-app>.onrender.com/api/telemetry/latest'`,
   `wsUrl:  'wss://<your-app>.onrender.com/ws'` — commit on GitHub and
   Vercel redeploys itself.

Notes: Render's free tier sleeps after ~15 min idle (first request
takes ~30 s to wake) and its disk resets on redeploys, so telemetry.db
is not permanent there. Fine for demos; add a Render persistent disk
for real history.

## Dashboard features (v1.2)
Geofencing with entry/exit alerts · historical playback with scrubber ·
alert center (bell icon) · CSV/JSON export · device commands (report
interval, find-buzzer, GNSS cold restart) · activity classification
(stationary/walking/vehicle) · presence detection while stationary ·
AS7341 white-point calibration (CAL button) · battery health (cycles +
capacity estimate) · daily summary strip.

## Runtime smoke-test (no browser needed)
```bash
npm install        # installs jsdom (dev only)
npm test           # boots the real dashboard script headlessly,
                   # feeds demo packets, asserts the UI updates
```

## Production notes
- Put the server behind HTTPS (nginx/caddy) — geolocation in the
  MY LOCATION feature requires a secure origin.
- Tighten CORS `allow_origins` in server.py to your dashboard origin.
- `demo-data.js` can be deleted in production; the dashboard degrades
  gracefully to live-only.
