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
    # the root is configurable (MQTT_BASE) and must match the firmware's
    # TOPIC_ROOT, so read it rather than hardcoding it here
    root = b.BASE
    assert parse_topic(f"{root}/TRK-01/telemetry") == ("TRK-01", "telemetry")
    assert parse_topic(f"{root}/TRK-01/cfg/geofence") == ("TRK-01", "cfg/geofence")
    assert parse_topic(f"{root}/TRK-01") == ("", "")              # too short
    assert parse_topic("someone-elses-root/TRK-01/telemetry") == ("", "")
    assert parse_topic("") == ("", "")
    assert topic_for("TRK-01", "cmd") == f"{root}/TRK-01/cmd"


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
    # the firmware drops any command without the shared secret, so every
    # payload the bridge builds must carry it
    if b.SECRET:
        assert d["k"] == b.SECRET
    assert json.loads(cmd_payload("find", 30, secret=""))\
        .get("k") is None, "an empty secret must omit the field, not send ''"
    assert json.loads(cmd_payload("gnss_restart"))["value"] is None
    assert json.loads(cmd_payload("clear_accident"))["cmd"] == "clear_accident"
    try:
        cmd_payload("rm -rf /")
    except ValueError:
        pass
    else:
        raise AssertionError("unknown commands must be rejected, not forwarded")


def test_fence_payload_single():
    # armed, accepting both the dashboard's keys (lng/r) and the wire keys
    d = json.loads(fence_payload({"lat": 30.354, "lng": 76.3637, "r": 200}))
    assert d["enabled"] and len(d["zones"]) == 1
    assert d["zones"][0]["lat"] == 30.354 and d["zones"][0]["lon"] == 76.3637
    assert d["zones"][0]["radius_m"] == 200.0
    # the flat keys stay alongside, so an older firmware build still reads
    # zone 1 instead of silently disarming itself
    assert d["lat"] == 30.354 and d["radius_m"] == 200.0

    d = json.loads(fence_payload({"lat": 1.0, "lon": 2.0, "radius_m": 50}))
    assert d["enabled"] and d["zones"][0]["radius_m"] == 50.0


def test_fence_payload_zones():
    zones = [{"name": "home", "lat": 30.3, "lng": 76.3, "r": 150},
             {"name": "office", "lat": 30.7, "lng": 76.7, "r": 300}]
    d = json.loads(fence_payload({"zones": zones}))
    assert d["enabled"] and len(d["zones"]) == 2
    if b.SECRET:
        assert d["k"] == b.SECRET
    assert [z["name"] for z in d["zones"]] == ["home", "office"]
    # a bare list works too
    assert len(json.loads(fence_payload(zones))["zones"]) == 2

    # names are defaulted and length-capped to what the firmware's char[16] holds
    d = json.loads(fence_payload([{"lat": 1, "lon": 2, "r": 10}]))
    assert d["zones"][0]["name"] == "zone 1"
    d = json.loads(fence_payload([{"name": "x" * 40, "lat": 1, "lon": 2, "r": 10}]))
    assert len(d["zones"][0]["name"]) <= 15

    # more than the firmware can hold must be dropped HERE, so the device
    # and the dashboard never disagree about what is armed
    many = [{"lat": i, "lon": i, "r": 100} for i in range(1, 9)]
    assert len(json.loads(fence_payload(many))["zones"]) == 4

    # junk zones are skipped, not allowed to disarm the good ones
    d = json.loads(fence_payload([{"lat": 1, "lon": 2, "r": 100},
                                  {"lat": "nonsense"}, None, {"r": 0}]))
    assert d["enabled"] and len(d["zones"]) == 1


def test_fence_payload_disarm():
    # disarm must be explicit and unambiguous — never a half-filled fence
    for empty in (None, {}, [], {"zones": []}, {"lat": 1, "lon": 2, "r": 0}):
        d = json.loads(fence_payload(empty))
        assert d["enabled"] is False and d["zones"] == [], empty
        # a disarm the device would reject is worse than no disarm at all
        if b.SECRET:
            assert d["k"] == b.SECRET, empty


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"  {name} ok")
    print("OK — bridge logic sound")
