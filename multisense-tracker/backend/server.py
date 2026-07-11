"""
=====================================================================
 MultiSense Tracker Console — BACKEND
=====================================================================
 FastAPI server that:
   · receives telemetry packets from the device   POST /api/telemetry
   · serves the newest packet to the dashboard    GET  /api/telemetry/latest
   · serves recent history for chart backfill     GET  /api/telemetry/history?limit=120
   · pushes every new packet to open dashboards   WS   /ws
   · lists known devices                          GET  /api/devices
   · clears stored telemetry (maintenance)        DELETE /api/telemetry
   · hosts the frontend (index.html + demo-data.js) at /

 Storage: SQLite file next to this script (telemetry.db). Every packet
 is kept verbatim as JSON, indexed by timestamp and device id.

 Security: set the TRACKER_API_KEY environment variable to require the
 device (and any DELETE calls) to send an  X-API-Key  header. Leave it
 unset during bring-up to disable auth.

 Run:
   pip install -r requirements.txt
   uvicorn server:app --host 0.0.0.0 --port 8000
 Then open  http://<server-ip>:8000  in a browser.
=====================================================================
"""

import csv
import io
import json
import os
import sqlite3
import time
from collections import defaultdict, deque
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, Header, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, PlainTextResponse
from fastapi.staticfiles import StaticFiles

# ---------------------------------------------------------------- paths / config
BASE_DIR = Path(__file__).resolve().parent
DB_PATH = BASE_DIR / "telemetry.db"
FRONTEND_DIR = BASE_DIR.parent / "frontend"
API_KEY = os.environ.get("TRACKER_API_KEY", "")          # "" → auth disabled
MAX_ROWS = int(os.environ.get("TRACKER_MAX_ROWS", "50000"))  # ring-buffer size

REQUIRED_FIELDS = {"gnss", "cell", "tof", "spectral", "battery"}

# ---------------------------------------------------------------- database
def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """CREATE TABLE IF NOT EXISTS packets(
               id        INTEGER PRIMARY KEY AUTOINCREMENT,
               ts        INTEGER NOT NULL,
               device_id TEXT    NOT NULL,
               payload   TEXT    NOT NULL)"""
    )
    conn.execute("CREATE INDEX IF NOT EXISTS idx_packets_ts ON packets(ts)")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_packets_dev ON packets(device_id)")
    return conn


def trim_ring_buffer(conn: sqlite3.Connection) -> None:
    """Keep the table bounded so an always-on device can't fill the disk."""
    conn.execute(
        "DELETE FROM packets WHERE id <= "
        "(SELECT COALESCE(MAX(id),0) - ? FROM packets)",
        (MAX_ROWS,),
    )


# ---------------------------------------------------------------- app
app = FastAPI(title="MultiSense Tracker API", version="1.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],          # tighten to your dashboard origin in production
    allow_methods=["*"],
    allow_headers=["*"],
)

ws_clients: set[WebSocket] = set()


def check_key(x_api_key: Optional[str]) -> None:
    if API_KEY and x_api_key != API_KEY:
        raise HTTPException(status_code=401, detail="invalid or missing X-API-Key")


# ---------------------------------------------------------------- ingest
@app.post("/api/telemetry")
async def ingest_packet(packet: dict, x_api_key: Optional[str] = Header(None)):
    """Device → server. Body must be one JSON telemetry packet."""
    check_key(x_api_key)

    missing = REQUIRED_FIELDS - packet.keys()
    if missing:
        raise HTTPException(422, f"packet missing fields: {sorted(missing)}")

    packet.setdefault("device_id", "TRK-7070-01")
    # Devices without an RTC may send ts=0 / seconds — normalise to ms epoch.
    ts = packet.get("ts", 0)
    if not isinstance(ts, (int, float)) or ts < 1e12:
        packet["ts"] = int(time.time() * 1000)

    conn = db()
    conn.execute(
        "INSERT INTO packets(ts, device_id, payload) VALUES (?,?,?)",
        (int(packet["ts"]), str(packet["device_id"]), json.dumps(packet)),
    )
    trim_ring_buffer(conn)
    conn.commit()
    conn.close()

    # fan out to every connected dashboard
    text = json.dumps(packet)
    dead = []
    for client in ws_clients:
        try:
            await client.send_text(text)
        except Exception:
            dead.append(client)
    for client in dead:
        ws_clients.discard(client)

    return {"ok": True, "ts": packet["ts"]}


# ---------------------------------------------------------------- reads
@app.get("/api/telemetry/latest")
def latest(device_id: Optional[str] = None):
    conn = db()
    if device_id:
        row = conn.execute(
            "SELECT payload FROM packets WHERE device_id=? ORDER BY id DESC LIMIT 1",
            (device_id,),
        ).fetchone()
    else:
        row = conn.execute(
            "SELECT payload FROM packets ORDER BY id DESC LIMIT 1"
        ).fetchone()
    conn.close()
    if not row:
        raise HTTPException(404, "no telemetry received yet")
    return JSONResponse(content=json.loads(row[0]))


@app.get("/api/telemetry/history")
def history(
    limit: int = Query(120, ge=1, le=2000),
    device_id: Optional[str] = None,
):
    """Most recent packets, returned oldest → newest (chart-friendly)."""
    conn = db()
    if device_id:
        rows = conn.execute(
            "SELECT payload FROM packets WHERE device_id=? ORDER BY id DESC LIMIT ?",
            (device_id, limit),
        ).fetchall()
    else:
        rows = conn.execute(
            "SELECT payload FROM packets ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
    conn.close()
    return JSONResponse(content=[json.loads(r[0]) for r in reversed(rows)])


@app.get("/api/devices")
def devices():
    conn = db()
    rows = conn.execute(
        "SELECT device_id, MAX(ts), COUNT(*) FROM packets GROUP BY device_id"
    ).fetchall()
    conn.close()
    return [
        {"device_id": r[0], "last_seen_ts": r[1], "packet_count": r[2]} for r in rows
    ]


# ---------------------------------------------------------------- device commands
# Dashboard queues commands; the device drains the queue by polling
# /api/command/next each report cycle (see the firmware sketch).
command_queues: dict[str, deque] = defaultdict(deque)
ALLOWED_CMDS = {"set_interval", "buzzer", "gnss_restart"}


@app.post("/api/command")
async def queue_command(body: dict):
    cmd = body.get("cmd")
    if cmd not in ALLOWED_CMDS:
        raise HTTPException(422, f"cmd must be one of {sorted(ALLOWED_CMDS)}")
    dev = str(body.get("device_id", "TRK-7070-01"))
    q = command_queues[dev]
    q.append({"cmd": cmd, "value": body.get("value"), "queued_ts": int(time.time() * 1000)})
    while len(q) > 20:
        q.popleft()
    return {"ok": True, "pending": len(q)}


@app.get("/api/command/next")
def next_command(device_id: str = "TRK-7070-01"):
    q = command_queues.get(device_id)
    if not q:
        return JSONResponse(content={"cmd": None})
    return JSONResponse(content=q.popleft())


@app.get("/api/command/pending")
def pending_commands(device_id: str = "TRK-7070-01"):
    return list(command_queues.get(device_id, []))


# ---------------------------------------------------------------- CSV export
@app.get("/api/telemetry/export.csv")
def export_csv(limit: int = Query(1000, ge=1, le=50000), device_id: Optional[str] = None):
    conn = db()
    if device_id:
        rows = conn.execute(
            "SELECT payload FROM packets WHERE device_id=? ORDER BY id DESC LIMIT ?",
            (device_id, limit)).fetchall()
    else:
        rows = conn.execute(
            "SELECT payload FROM packets ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
    conn.close()
    out = io.StringIO()
    w = csv.writer(out)
    w.writerow(["ts", "iso", "device_id", "lat", "lon", "fix", "sats", "hdop", "speed_kmh",
                "rssi_dbm", "centre_mm", "min_mm", "max_mm", "lux", "clear", "nir",
                "voltage_v", "current_ma", "soc_pct", "charging"])
    for (payload,) in reversed(rows):
        p = json.loads(payload)
        g, c, t, sp, b = p.get("gnss", {}), p.get("cell", {}), p.get("tof", {}), p.get("spectral", {}), p.get("battery", {})
        w.writerow([p.get("ts"), time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(p.get("ts", 0)/1000)),
                    p.get("device_id"), g.get("lat"), g.get("lon"), int(bool(g.get("fix"))),
                    g.get("sats"), g.get("hdop"), g.get("speed_kmh"), c.get("rssi_dbm"),
                    t.get("distance_mm"), t.get("min_mm"), t.get("max_mm"),
                    sp.get("lux"), sp.get("clear"), sp.get("nir"),
                    b.get("voltage_v"), b.get("current_ma"), b.get("soc_pct"),
                    int(bool(b.get("charging")))])
    return PlainTextResponse(out.getvalue(), media_type="text/csv",
        headers={"Content-Disposition": "attachment; filename=multisense-telemetry.csv"})


# ---------------------------------------------------------------- maintenance
@app.delete("/api/telemetry")
def clear(x_api_key: Optional[str] = Header(None)):
    check_key(x_api_key)
    conn = db()
    conn.execute("DELETE FROM packets")
    conn.commit()
    conn.close()
    return {"ok": True, "cleared": True}


# ---------------------------------------------------------------- websocket
@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    ws_clients.add(ws)
    # greet the new dashboard with the latest packet so it paints instantly
    try:
        conn = db()
        row = conn.execute(
            "SELECT payload FROM packets ORDER BY id DESC LIMIT 1"
        ).fetchone()
        conn.close()
        if row:
            await ws.send_text(row[0])
        while True:
            await ws.receive_text()  # ignore client pings/keepalives
    except WebSocketDisconnect:
        pass
    except Exception:
        pass
    finally:
        ws_clients.discard(ws)


# ---------------------------------------------------------------- frontend
# Mounted last so /api and /ws routes take precedence.
app.mount("/", StaticFiles(directory=FRONTEND_DIR, html=True), name="frontend")
