#!/usr/bin/env python3
"""Reset the kit and print its serial log as it arrives, for N seconds.

usage: capture_log.py PORT SECONDS      (needs pyserial; the ESP-IDF Python env has it)
Opening the port resets the board, so start this before the thing you want to log."""
import sys, time
import serial

port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = s.rts = False
s.open()
s.rts = True; time.sleep(0.1); s.rts = False   # pulse EN: normal boot
end = time.time() + secs
while time.time() < end:
    sys.stdout.write(s.read(4096).decode("utf-8", "replace"))
    sys.stdout.flush()
s.close()
