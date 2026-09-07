#!/usr/bin/env python3
"""Provision a BEACON device over its USB serial console.

The device exposes `beacon-set` / `beacon-save` / `beacon-reboot` at a
`beacon>` prompt. Driving that by hand means running an interactive monitor and
remembering four commands, so this does it in one step and reads the Wi-Fi
password from a prompt rather than argv - keeping it out of shell history and
out of the process list.

    tools/provision.py --ssid MY-NETWORK
    tools/provision.py --ssid MY-NETWORK --hub http://HUB_HOST:8787
    tools/provision.py --show          # just print the current configuration

Requires pyserial (the repo venv has it).
"""

from __future__ import annotations

import argparse
import getpass
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

PROMPT = b"beacon>"


def reset(port: serial.Serial) -> None:
    """Pulse the chip's reset the way esptool does.

    Two reasons. The console prompt is printed once, early, so catching it
    reliably means starting from a known boot. And an idle USB-Serial/JTAG
    endpoint can stall - pyserial then raises "device reports readiness to
    read but returned no data" - which a re-enumeration clears."""
    port.setDTR(False)
    port.setRTS(True)
    time.sleep(0.12)
    port.setRTS(False)
    time.sleep(0.05)
    port.reset_input_buffer()
    # Host->device bytes are only delivered while DTR is asserted.
    port.setDTR(True)


def read_some(port: serial.Serial, size: int = 4096) -> bytes:
    try:
        return port.read(size)
    except serial.SerialException:
        # A stalled endpoint surfaces here; treat it as "nothing yet" and let
        # the caller's timeout decide.
        time.sleep(0.2)
        return b""


def drain(port: serial.Serial, seconds: float, echo: bool = False) -> str:
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        chunk = read_some(port)
        if chunk:
            out += chunk
            if echo:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
    return out.decode("utf-8", "replace")


def wait_for_prompt(port: serial.Serial, timeout: float = 25.0) -> bool:
    """The prompt only appears a few seconds into boot, after the panel's
    first refresh, so this can take a while on a cold start."""
    out = b""
    end = time.time() + timeout
    while time.time() < end:
        out += read_some(port)
        if PROMPT in out:
            return True
    return PROMPT in out


def send(port: serial.Serial, line: str, quiet: str | None = None,
         timeout: float = 6.0) -> str:
    """Send one command and read until the prompt comes back.

    A fixed sleep is not enough: the device interleaves its own log lines with
    console output, and a panel refresh can hold the main task for over a
    second, so the reply may arrive well after the command echo."""
    port.reset_input_buffer()
    port.write((line + "\r\n").encode())
    port.flush()

    shown = quiet if quiet is not None else line
    print(f"  > {shown}")

    raw = b""
    end = time.time() + timeout
    while time.time() < end:
        raw += read_some(port)
        # The echoed command is followed by output and then a fresh prompt.
        if raw.count(PROMPT) >= 1 and raw.rstrip().endswith(PROMPT):
            break
    reply = raw.decode("utf-8", "replace")
    for text in reply.replace("beacon>", "\n").splitlines():
        text = text.strip()
        if not text or text == line:
            continue
        if quiet is not None and text.startswith(line.split()[0]):
            continue           # never echo a secret back
        print(f"    {text}")
    return reply


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--ssid")
    ap.add_argument("--hub", help="e.g. http://192.168.1.10:8787")
    ap.add_argument("--token")
    ap.add_argument("--password", help="prompted for if omitted")
    ap.add_argument("--show", action="store_true",
                    help="print the current configuration and exit")
    ap.add_argument("--no-reboot", action="store_true")
    ap.add_argument("--watch", type=float, default=25.0,
                    help="seconds to watch the log after rebooting")
    args = ap.parse_args()

    if not args.show and not any((args.ssid, args.hub, args.token)):
        ap.error("nothing to do: pass --ssid / --hub / --token, or --show")

    password = None
    if args.ssid:
        password = args.password
        if password is None:
            password = getpass.getpass(f"Wi-Fi password for {args.ssid}: ")

    port = serial.Serial(args.port, 115200, timeout=0.2, write_timeout=3.0)
    print(f"resetting the device and waiting for its console on {args.port}…")
    reset(port)
    if not wait_for_prompt(port):
        print("no beacon> prompt. Is this a BEACON build, and is the device "
              "not already open in another monitor?", file=sys.stderr)
        return 1
    print("console ready\n")

    if args.show:
        send(port, "beacon-show")
        return 0

    if args.ssid:
        send(port, f"beacon-set ssid {args.ssid}")
        send(port, f"beacon-set pass {password}",
             quiet="beacon-set pass ********")
    if args.hub:
        send(port, f"beacon-set hub {args.hub}")
    if args.token:
        send(port, f"beacon-set token {args.token}",
             quiet="beacon-set token ********")

    send(port, "beacon-save")
    send(port, "beacon-show")

    if args.no_reboot:
        return 0

    print("\nrebooting…\n")
    port.write(b"beacon-reboot\r\n")
    port.flush()
    log = drain(port, args.watch, echo=True)

    print()
    if "beacon.net: ip " in log:
        print("network up.")
        if "hub returned" in log or "poll failed" in log:
            print("…but the hub did not answer. Check that it is running and "
                  "that this network can reach it - an IoT SSID is often "
                  "isolated from the LAN the hub is on.")
            return 2
        return 0
    if "waiting for wi-fi" in log:
        print("still joining. Watch it with:  tools/idf.sh -p %s monitor"
              % args.port)
        return 2
    print("no network log seen; watch it with:  tools/idf.sh -p %s monitor"
          % args.port)
    return 2


if __name__ == "__main__":
    sys.exit(main())
