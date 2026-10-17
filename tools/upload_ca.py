#!/usr/bin/env python3
"""
Upload the CA certificate into the A7672S once. It lives in the modem's own
flash and survives reboots and ESP32 reflashes, so this is a one-time job.

    pip install pyserial
    python3 tools/upload_ca.py --port COM5            # Windows
    python3 tools/upload_ca.py --port /dev/ttyUSB0    # Linux / macOS

Wire a USB-TTL adapter straight to the A7672S UART (or run the passthrough
sketch in docs/SETUP.md). The ESP32 must NOT be driving that UART at the
same time.

THE GOTCHA THIS SCRIPT EXISTS FOR: the AT command lines end with CRLF, but
the certificate body must be sent with BARE LF. Upload it with CRLF and the
modem stores a cert it cannot parse, then every AT+CMQTTCONNECT fails with
"+CMQTTCONNECT: 0,34" and nothing tells you why. This script normalises the
line endings and declares the byte count after normalising.
"""
import argparse
import pathlib
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing:  pip install pyserial")

DEFAULT_PEM = pathlib.Path(__file__).resolve().parent.parent / "certs" / "isrgrootx1.pem"


def expect(ser, want, timeout=10.0, echo=True):
    """Read until `want` appears. Returns the text read."""
    buf, deadline = "", time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1).decode("utf-8", "replace")
        if chunk:
            buf += chunk
            if echo:
                sys.stdout.write(chunk)
                sys.stdout.flush()
            if want in buf:
                return buf
        else:
            time.sleep(0.05)
    return buf


def at(ser, cmd, want="OK", timeout=10.0):
    print(f"\n>> {cmd}")
    ser.write((cmd + "\r\n").encode())
    got = expect(ser, want, timeout)
    if want not in got:
        sys.exit(f"\n!! expected {want!r} after {cmd!r} — got:\n{got}")
    return got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--pem", type=pathlib.Path, default=DEFAULT_PEM)
    ap.add_argument("--name", default="isrgrootx1.pem",
                    help="filename inside the modem; must match MQTT_CA_FILE in config.h")
    ap.add_argument("--replace", action="store_true", help="delete an existing copy first")
    args = ap.parse_args()

    # Normalise to bare LF, and count the bytes AFTER normalising.
    body = args.pem.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    if not body.endswith(b"\n"):
        body += b"\n"
    if b"BEGIN CERTIFICATE" not in body:
        sys.exit(f"{args.pem} does not look like a PEM certificate")
    print(f"{args.pem.name}: {len(body)} bytes (LF endings) -> modem as {args.name!r}")

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()
        at(ser, "AT")
        at(ser, "ATE0")

        listing = at(ser, "AT+CCERTLIST")
        if args.name in listing:
            if not args.replace:
                print(f"\n'{args.name}' is already in the modem. "
                      f"Re-run with --replace to overwrite it.")
                return
            at(ser, f'AT+CCERTDELE="{args.name}"')

        print(f"\n>> AT+CCERTDOWN=\"{args.name}\",{len(body)}")
        ser.write(f'AT+CCERTDOWN="{args.name}",{len(body)}\r\n'.encode())
        if ">" not in expect(ser, ">", 10):
            sys.exit("\n!! no '>' data prompt — wrong baud, or the modem is asleep")

        ser.write(body)              # raw, LF only, no trailing CR
        ser.flush()
        if "OK" not in expect(ser, "OK", 20):
            sys.exit("\n!! modem did not accept the certificate")

        final = at(ser, "AT+CCERTLIST")
        if args.name in final:
            print(f"\n\nDone — '{args.name}' is stored in the modem and will "
                  f"survive reboots.\nSet MQTT_CA_FILE to \"{args.name}\" in config.h.")
        else:
            sys.exit("\n!! upload reported OK but the file is not listed")


if __name__ == "__main__":
    main()
