#!/usr/bin/env python3
"""Long-running, reset-safe serial logger for the Guition chart board.

Timestamps every line, never pulses EN (RTS) or IO0 (DTR) after open, and
reopens the port if it re-enumerates. Usage:
    serial_soak_logger.py /dev/ttyUSB0 logs/soak.log
"""
import datetime
import sys
import time

import serial


def stamp() -> str:
    return datetime.datetime.now().isoformat(timespec="milliseconds")


def run(port: str, out_path: str) -> None:
    with open(out_path, "a", buffering=1, encoding="utf-8", errors="replace") as out:
        out.write(f"{stamp()} [logger] start {port}\n")
        partial = ""
        while True:
            try:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout = port, 115200, 0.5
                s.dtr = False
                s.rts = False  # RTS drives EN: keep it released so the board runs
                s.open()
                out.write(f"{stamp()} [logger] opened\n")
                while True:
                    chunk = s.read(4096).decode("utf-8", errors="replace")
                    if not chunk:
                        continue
                    partial += chunk.replace("\r", "")
                    *lines, partial = partial.split("\n")
                    for line in lines:
                        out.write(f"{stamp()} {line}\n")
            except (serial.SerialException, OSError) as exc:
                out.write(f"{stamp()} [logger] port error: {exc}; retrying\n")
                time.sleep(2)


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
