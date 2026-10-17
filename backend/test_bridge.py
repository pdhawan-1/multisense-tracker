"""Self-check for the bridge's pure logic. No broker, no network, no pytest.

    python3 test_bridge.py        ->  prints OK, exits 0

These four functions are where a wrong answer is silent: a mis-parsed
topic drops every packet, a bad ts puts the whole trail in 1970, and a
malformed fence payload disarms a geofence that the user thinks is armed.
"""
import json
import time

import mqtt_bridge as b
from mqtt_bridge import (cmd_payload, fence_payload, normalize_packet,
                         parse_topic, topic_for)


def test_parse_topic():
    assert parse_topic("multisense/TRK-01/telemetry") == ("TRK-01", "telemetry")
    assert parse_topic("multisense/TRK-01/cfg/geofence") == ("TRK-01", "cfg/geofence")
    assert parse_topic("multisense/TRK-01") == ("", "")          # too short
    assert parse_topic("other/TRK-01/telemetry") == ("", "")     # wrong root
    assert parse_topic("") == ("", "")
    assert topic_for("TRK-01", "cmd") == f"{b.BASE}/TRK-01/cmd"


def test_normalize_ts():
    now_ms = int(time.time() * 1000)

    # device sends ts=0 (no RTC) -> stamped on arrival
    p = normalize_packet({"ts": 0}, "TRK-09")
    assert abs(p["ts"] - now_ms) < 5000, p["ts"]
    assert p["device_id"] == "TRK-09"

    # seconds-epoch is not a ms-epoch, so it must be replaced, not trusted
    p = normalize_packet({"ts": 1720512000})
    assert abs(p["ts"] - now_ms) < 5000

    # a real ms-epoch is preserved exactly (replayed offline packets rely on this)
    p = normalize_packet({"ts": 1720512000000})
    assert p["ts"] == 1720512000000

    # garbage types must not crash or poison the DB
    for bad in (None, "abc", [], {}):
        assert abs(normalize_packet({"ts": bad})["ts"] - now_ms) < 5000

    # an explicit device_id in the payload wins over the topic's
    p = normalize_packet({"ts": 0, "device_id": "FROM-PAYLOAD"}, "FROM-TOPIC")
    assert p["device_id"] == "FROM-PAYLOAD"


def test_cmd_payload():
    d = json.loads(cmd_payload("find", 30))
    assert d["cmd"] == "find" and d["value"] == 30 and d["queued_ts"] > 0
    assert json.loads(cmd_payload("gnss_restart"))["value"] is None
    try:
        cmd_payload("rm -rf /")
    except ValueError:
        pass
    else:
        raise AssertionError("unknown commands must be rejected, not forwarded")


def test_fence_payload():
    # armed, accepting both the dashboard's keys (lng/r) and the wire keys
    d = json.loads(fence_payload({"lat": 30.354, "lng": 76.3637, "r": 200}))
    assert d == {"enabled": True, "lat": 30.354, "lon": 76.3637, "radius_m": 200.0}
    d = json.loads(fence_payload({"lat": 1.0, "lon": 2.0, "radius_m": 50}))
    assert d["enabled"] and d["radius_m"] == 50.0

    # disarm must be explicit and unambiguous — never a half-filled fence
    assert json.loads(fence_payload(None)) == {"enabled": False}
    assert json.loads(fence_payload({}))["enabled"] is False
    assert json.loads(fence_payload({"lat": 1, "lon": 2, "r": 0}))["enabled"] is False


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"  {name} ok")
    print("OK — bridge logic sound")
