"""
=====================================================================
 mqtt_bridge.py — HiveMQ Cloud <-> FastAPI bridge
=====================================================================
 Subscribes to the device topics and hands every telemetry packet to
 the SAME sink the HTTP ingest route uses, so SQLite history, the
 /ws fan-out, CSV export and playback all keep working untouched.

 Also publishes the two downlink messages:
   · commands        multisense/<dev>/cmd            (not retained)
   · geofence config multisense/<dev>/cfg/geofence   (RETAINED, so a
     device that boots hours later still gets the current fence)

 Configure with environment variables — never hard-code the password:
   MQTT_HOST=broker.emqx.io
   MQTT_PORT=1883
   MQTT_TLS=0                    (1 for a TLS broker on 8883)
   MQTT_USER=                    (empty for an open broker)
   MQTT_PASS=
   MQTT_BASE=ms-89a4f93827c1     MUST match TOPIC_ROOT in the firmware
   MQTT_SECRET=...               MUST match CMD_SECRET in the firmware
 Leave MQTT_HOST unset and the bridge stays dormant — the server then
 behaves exactly as it did before, HTTP ingest only.
=====================================================================
"""

import asyncio
import json
import logging
import os
import ssl
import time
from typing import Any, Callable, Optional

log = logging.getLogger("mqtt_bridge")

HOST = os.environ.get("MQTT_HOST", "")
PORT = int(os.environ.get("MQTT_PORT", "1883"))
TLS = os.environ.get("MQTT_TLS", "0") not in ("0", "false", "False", "")
USER = os.environ.get("MQTT_USER", "")
PASS = os.environ.get("MQTT_PASS", "")
BASE = os.environ.get("MQTT_BASE", "ms-89a4f93827c1")
# Shared secret the firmware requires on every downlink. Not real auth —
# it only stops a stranger who guessed the topic on a public broker.
SECRET = os.environ.get("MQTT_SECRET", "RCxM5n3tiwlYoPDZlbMrnhrS")

_client = None
_loop: Optional[asyncio.AbstractEventLoop] = None
_sink: Optional[Callable] = None
_connected = False

ALLOWED_CMDS = {"set_interval", "buzzer", "find", "gnss_restart", "mute",
                "led_test", "reboot", "clear_accident"}


# ------------------------------------------------------------------ pure helpers
# Kept free of I/O so test_bridge.py can assert on them without a broker.

def parse_topic(topic: str) -> tuple[str, str]:
    """'multisense/TRK-01/telemetry' -> ('TRK-01', 'telemetry'). ('', '') if junk."""
    parts = topic.split("/")
    if len(parts) < 3 or parts[0] != BASE:
        return "", ""
    return parts[1], "/".join(parts[2:])


def normalize_packet(packet: dict, device_id: str = "") -> dict:
    """Fill in what the device cannot know. Shared by HTTP and MQTT ingest."""
    if device_id:
        packet.setdefault("device_id", device_id)
    packet.setdefault("device_id", "TRK-A7672S-01")
    ts = packet.get("ts", 0)
    # The tracker has no RTC and sends ts=0; anything that isn't a
    # plausible ms-epoch gets stamped on arrival.
    if not isinstance(ts, (int, float)) or ts < 1e12:
        packet["ts"] = int(time.time() * 1000)
    else:
        packet["ts"] = int(ts)
    return packet


def cmd_payload(cmd: str, value: Any = None, secret: Optional[str] = None) -> str:
    if cmd not in ALLOWED_CMDS:
        raise ValueError(f"cmd must be one of {sorted(ALLOWED_CMDS)}")
    body = {"cmd": cmd, "value": value, "queued_ts": int(time.time() * 1000)}
    k = SECRET if secret is None else secret
    if k:
        body["k"] = k          # the firmware drops any command without this
    return json.dumps(body)


FENCE_MAX_ZONES = 4


def _one_zone(z: dict, fallback_name: str) -> Optional[dict]:
    """Accept the dashboard's keys (lng/r) or the wire keys (lon/radius_m)."""
    try:
        lat = float(z["lat"])
        lon = float(z.get("lon", z.get("lng")))
        rad = float(z.get("radius_m", z.get("r", 0)))
    except (KeyError, TypeError, ValueError):
        return None
    if rad <= 0:
        return None
    return {"name": str(z.get("name") or fallback_name)[:15],
            "lat": lat, "lon": lon, "radius_m": rad}


def fence_payload(fence) -> str:
    """Build the retained geofence config for the device.

    Takes a list of zones, a dict with a 'zones' list, a single-zone dict
    (the old form), or None/empty to disarm everything. The firmware caps
    it at FENCE_MAX_ZONES; anything beyond that is dropped here so the
    device and the dashboard agree on what is armed.
    """
    if not fence:
        off = {"enabled": False, "zones": []}
        if SECRET:
            off["k"] = SECRET
        return json.dumps(off)

    if isinstance(fence, dict) and "zones" in fence:
        raw = fence.get("zones") or []
    elif isinstance(fence, list):
        raw = fence
    else:
        raw = [fence]

    zones = []
    for i, z in enumerate(raw):
        if not isinstance(z, dict):
            continue
        parsed = _one_zone(z, f"zone {i + 1}")
        if parsed:
            zones.append(parsed)
        if len(zones) >= FENCE_MAX_ZONES:
            break

    if not zones:
        off = {"enabled": False, "zones": []}
        if SECRET:
            off["k"] = SECRET
        return json.dumps(off)

    # The single-zone keys are kept alongside so an older firmware build
    # still reads zone 1 instead of silently disarming.
    out = {"enabled": True, "zones": zones,
           "lat": zones[0]["lat"], "lon": zones[0]["lon"],
           "radius_m": zones[0]["radius_m"]}
    if SECRET:
        out["k"] = SECRET
    return json.dumps(out)


def topic_for(device_id: str, leaf: str) -> str:
    return f"{BASE}/{device_id}/{leaf}"


# ------------------------------------------------------------------ client
def _build_client():
    import paho.mqtt.client as mqtt
    cid = f"backend-{os.getpid()}"
    try:                                     # paho-mqtt 2.x
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid,
                        protocol=mqtt.MQTTv311)
    except (AttributeError, TypeError):      # paho-mqtt 1.x
        c = mqtt.Client(client_id=cid, protocol=mqtt.MQTTv311)
    if USER:
        c.username_pw_set(USER, PASS)
    if TLS:
        # Hosted brokers serve a public chain; the OS trust store covers it.
        c.tls_set(cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    return c


def _on_connect(client, userdata, flags, rc, properties=None):
    global _connected
    ok = (getattr(rc, "value", rc) == 0)
    _connected = ok
    if not ok:
        log.error("MQTT connect refused: %s", rc)
        return
    log.info("MQTT bridge connected to %s", HOST)
    client.subscribe([(f"{BASE}/+/telemetry", 1),
                      (f"{BASE}/+/event", 1),
                      (f"{BASE}/+/status", 1)])


def _on_disconnect(client, userdata, *args, **kw):
    global _connected
    _connected = False
    log.warning("MQTT bridge disconnected — paho will retry")


def _on_message(client, userdata, msg):
    device_id, leaf = parse_topic(msg.topic)
    if not device_id:
        return
    try:
        body = json.loads(msg.payload.decode("utf-8", "replace"))
    except json.JSONDecodeError:
        log.warning("non-JSON on %s: %r", msg.topic, msg.payload[:80])
        return

    if leaf == "telemetry":
        packet = normalize_packet(body, device_id)
    elif leaf in ("event", "status"):
        # Forwarded to open dashboards but not stored as telemetry — it has
        # none of the required sensor sections.
        packet = {"_kind": leaf, "device_id": device_id,
                  "ts": int(time.time() * 1000), **body}
    else:
        return

    if _sink and _loop and not _loop.is_closed():
        asyncio.run_coroutine_threadsafe(_sink(packet, leaf), _loop)


def start(loop: asyncio.AbstractEventLoop, sink: Callable) -> bool:
    """sink is an async callable (packet: dict, leaf: str) -> None."""
    global _client, _loop, _sink
    if not HOST:
        log.info("MQTT_HOST unset — bridge disabled, HTTP ingest only")
        return False
    _loop, _sink = loop, sink
    _client = _build_client()
    _client.on_connect = _on_connect
    _client.on_disconnect = _on_disconnect
    _client.on_message = _on_message
    _client.reconnect_delay_set(min_delay=1, max_delay=60)
    _client.connect_async(HOST, PORT, keepalive=60)
    _client.loop_start()
    return True


def stop():
    if _client:
        _client.loop_stop()
        try:
            _client.disconnect()
        except Exception:
            pass


def connected() -> bool:
    return _connected


def publish_cmd(device_id: str, cmd: str, value: Any = None) -> bool:
    if not _client or not _connected:
        return False
    r = _client.publish(topic_for(device_id, "cmd"), cmd_payload(cmd, value), qos=1)
    return r.rc == 0


def publish_fence(device_id: str, fence) -> bool:
    """Retained: a device booting later still picks up the current zone."""
    if not _client or not _connected:
        return False
    r = _client.publish(topic_for(device_id, "cfg/geofence"),
                        fence_payload(fence), qos=1, retain=True)
    return r.rc == 0
