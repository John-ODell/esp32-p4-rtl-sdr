#!/usr/bin/env python3
"""Print the kit's serial log as it arrives, for N seconds.

usage: capture_log.py PORT SECONDS [--attach]   (needs pyserial; the ESP-IDF Python env has it)

Without --attach the board is reset first, so the log starts at boot. With --attach it is not:
the port is opened with DTR and RTS both asserted, which is what macOS does on open anyway, so
the auto-reset circuit sees no edge. If the USB serial port drops out (the kit's bridge does now
and then), the script reattaches the same way and carries on, so a long run is not reset."""
import sys, time
import serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
port, secs, attach = args[0], float(args[1]), "--attach" in sys.argv


def open_port(reset):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    if reset:
        s.dtr = s.rts = False
        s.open()
        s.rts = True; time.sleep(0.1); s.rts = False   # pulse EN: normal boot
    else:
        s.dtr = s.rts = True
        s.open()
    return s


end = time.time() + secs
s = open_port(not attach)
while time.time() < end:
    try:
        sys.stdout.write(s.read(4096).decode("utf-8", "replace"))
        sys.stdout.flush()
    except (serial.SerialException, OSError) as e:
        sys.stdout.write(f"\n[capture_log {time.strftime('%H:%M:%S')}: port dropped ({e}); reattaching]\n")
        try:
            s.close()
        except Exception:
            pass
        s = None
        while s is None and time.time() < end:
            time.sleep(0.5)
            try:
                s = open_port(False)
            except (serial.SerialException, OSError):
                pass
        if s is None:
            break
if s:
    s.close()
