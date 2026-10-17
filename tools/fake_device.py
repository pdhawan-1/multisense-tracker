#!/usr/bin/env python3
"""
Pretend to be the tracker. Publishes realistic packets to your broker so you
can prove the broker -> backend -> dashboard chain works BEFORE the hardware
does. Also subscribes to the cmd and geofence topics and prints whatever the
dashboard sends down, which is how you verify the find-device button and the
fence push without a buzzer in your hand.

    pip install paho-mqtt
    MQTT_HOST=xxxx.s1.eu.hivemq.cloud MQTT_USER=u MQTT_PASS=p \
        python3 tools/fake_device.py --interval 5
"""
import argparse
import json
import math
import os
import random
import ssl
import time

import paho.mqtt.client as mqtt

HOST = os.environ["MQTT_HOST"]
PORT = int(os.environ.get("MQTT_PORT", "8883"))
USER = os.environ.get("MQTT_USER", "")
PASS = os.environ.get("MQTT_PASS", "")
BASE = os.environ.get("MQTT_BASE", "multisense")
DEV = os.environ.get("DEVICE_ID", "TRK-A7672S-01")

LAT0, LON0 = 30.3540, 76.3637          # TIET Patiala


def packet(step: int) -> dict:
    """A slow circular walk, a plausible ToF scene and a draining 2S pack."""
    ang = step / 40.0
    lat = LAT0 + 0.0014 * math.sin(ang)
    lon = LON0 + 0.0014 * math.cos(ang)

    grid, zst = [], []
    for r in range(8):
        for c in range(8):
            d = 1400 + r * 25 + random.randint(-15, 15)
            if 2 <= r <= 5 and 1 <= c <= 4:      # an object in the left-centre
                d = 430 + (r - 2) * 40 + random.randint(-10, 10)
            grid.append(d)
            zst.append(5)

    soc = max(5, 92 - step // 6)
    vcell = 3.0 + (soc / 100) * 1.2              # crude inverse of the curve
    return {
        "ts": 0,                                  # no RTC, like the real device
        "device_id": DEV,
        "uptime_s": 60 + step * 5,
        "fw": "2.0.0-fake",
        "gnss": {"fix": True, "lat": round(lat, 6), "lon": round(lon, 6),
                 "alt_m": 251.0, "speed_kmh": round(random.uniform(0.5, 4.5), 1),
                 "sats": random.randint(7, 12), "hdop": round(random.uniform(0.8, 1.6), 1)},
        "cell": {"mode": "LTE", "operator": "Airtel IN",
                 "rssi_dbm": random.randint(-85, -62), "band": "EUTRAN-BAND3",
                 "registered": True},
        "tof": {"res": 8, "grid": grid, "zstatus": zst,
                "distance_mm": sum(grid[27:29] + grid[35:37]) // 4,
                "min_mm": min(grid), "max_mm": max(grid),
                "signal_mcps": 5.2, "mode": "8x8 @ 15 Hz"},
        "spectral": {"f415": 210, "f445": 320, "f480": 512, "f515": 640,
                     "f555": 780, "f590": 655, "f630": 540, "f680": 410,
                     "clear": 3120 + random.randint(-200, 200), "nir": 460,
                     "gain": "64x", "lux": 512},
        "battery": {"voltage_v": round(vcell * 2, 2),
                    "current_ma": -random.randint(110, 180),
                    "soc_pct": soc, "charging": False},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval", type=float, default=5.0)
    ap.add_argument("--count", type=int, default=0, help="0 = forever")
    args = ap.parse_args()

    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=DEV + "-fake")
    except (AttributeError, TypeError):
        c = mqtt.Client(client_id=DEV + "-fake")
    if USER:
        c.username_pw_set(USER, PASS)
    c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    c.will_set(f"{BASE}/{DEV}/status", json.dumps({"state": "offline"}),
               qos=1, retain=True)

    c.on_message = lambda cl, u, m: print(
        f"\n  <-- DOWNLINK {m.topic}\n      {m.payload.decode()}\n")

    c.connect(HOST, PORT, 60)
    c.subscribe([(f"{BASE}/{DEV}/cmd", 1), (f"{BASE}/{DEV}/cfg/#", 1)])
    c.publish(f"{BASE}/{DEV}/status", json.dumps({"state": "online"}),
              qos=1, retain=True)
    c.loop_start()
    print(f"publishing to {BASE}/{DEV}/telemetry every {args.interval}s  (Ctrl-C to stop)")

    step = 0
    try:
        while args.count == 0 or step < args.count:
            body = json.dumps(packet(step))
            c.publish(f"{BASE}/{DEV}/telemetry", body, qos=1)
            print(f"  --> packet {step + 1}  ({len(body)} bytes)")
            step += 1
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        c.publish(f"{BASE}/{DEV}/status", json.dumps({"state": "offline"}),
                  qos=1, retain=True)
        time.sleep(0.5)
        c.loop_stop()


if __name__ == "__main__":
    main()
