"""Bounded serial capture with an optional reset and injected console commands.

Notes earned the hard way on this board:
  * idf.py monitor is interactive, and USB-Serial/JTAG discards anything the
    device prints while no host is attached - so the port must be open before
    the chip resets. DTR/RTS is how esptool drives that reset.
  * write() must have a write_timeout: the CDC endpoint blocks forever if the
    device is not draining it, which silently hangs the capture.
  * Output is streamed to stdout as it arrives, so a hang still leaves a log.
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20
send = sys.argv[3] if len(sys.argv) > 3 else None
delay = float(sys.argv[4]) if len(sys.argv) > 4 else 6.0

s = serial.Serial(port, 115200, timeout=0.2, write_timeout=2.0)
s.setDTR(False); s.setRTS(True)
time.sleep(0.12)
s.setRTS(False); s.setDTR(False)
time.sleep(0.05)
s.reset_input_buffer()

end = time.time() + secs
pending = send.split(";;") if send else []
send_at = time.time() + delay
while time.time() < end:
    chunk = s.read(8192)
    if chunk:
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
    if pending and time.time() >= send_at:
        try:
            s.write((pending.pop(0) + "\r\n").encode())
            s.flush()
        except serial.SerialTimeoutException:
            sys.stdout.write("\n[monitor] write timed out\n")
        send_at = time.time() + 2.0
s.close()
