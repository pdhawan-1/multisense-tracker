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
   MQTT_HOST=xxxx.s1.eu.hivemq.cloud
   MQTT_PORT=8883
   MQTT_USER=backend-bridge
   MQTT_PASS=...
   MQTT_BASE=multisense          (optional, must match the firmware)
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
PORT = int(os.environ.get("MQTT_PORT", "8883"))
USER = os.environ.get("MQTT_USER", "")
PASS = os.environ.get("MQTT_PASS", "")
BASE = os.environ.get("MQTT_BASE", "multisense")

_client = None
_loop: Optional[asyncio.AbstractEventLoop] = None
_sink: Optional[Callable] = None
_connected = False

ALLOWED_CMDS = {"set_interval", "buzzer", "find", "gnss_restart", "mute",
                "led_test", "reboot"}


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


def cmd_payload(cmd: str, value: Any = None) -> str:
    if cmd not in ALLOWED_CMDS:
        raise ValueError(f"cmd must be one of {sorted(ALLOWED_CMDS)}")
    return json.dumps({"cmd": cmd, "value": value,
                       "queued_ts": int(time.time() * 1000)})


def fence_payload(fence: Optional[dict]) -> str:
    """fence = {'lat':.., 'lng'|'lon':.., 'r'|'radius_m':..} or None to disarm."""
    if not fence:
        return json.dumps({"enabled": False})
    lat = float(fence.get("lat"))
    lon = float(fence.get("lon", fence.get("lng")))
    rad = float(fence.get("radius_m", fence.get("r", 0)))
    if rad <= 0:
        return json.dumps({"enabled": False})
    return json.dumps({"enabled": True, "lat": lat, "lon": lon,
                       "radius_m": rad})


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
    # HiveMQ Cloud serves a Let's Encrypt chain; the OS trust store covers it.
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


def publish_fence(device_id: str, fence: Optional[dict]) -> bool:
    """Retained: a device booting later still picks up the current zone."""
    if not _client or not _connected:
        return False
    r = _client.publish(topic_for(device_id, "cfg/geofence"),
                        fence_payload(fence), qos=1, retain=True)
    return r.rc == 0
