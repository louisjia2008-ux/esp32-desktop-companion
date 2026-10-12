#!/usr/bin/env python3
"""Capture whitelisted real v0.9 sensor/runtime evidence. Never generates readings.

Opening the board's USB serial port can reset it. A second observed boot/fault
stops the capture; disconnect power manually if a hardware fault is reported.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import time
import serial

PREFIXES = (
    "SENSOR_", "LOCAL_CO2_REMINDER ", "METRICS ", "NETWORK_DIAG ",
    "TOUCH_", "EVENT_METRICS ", "BOOT_METRICS ", "BOOT_HEALTH ",
    "DEVCOMPANION_FW=", "DEVCOMPANION_BUILD=", "SD_WALLPAPER_METRICS ",
    "HOST_STATUS_ACCEPTED ", "PAGE ", "GESTURE ", "EVENT_", "OTA ",
    "WS_TRANSPORT ", "WebSocket connecting ", "WebSocket connected",
    "WebSocket disconnected", "Bridge pairing challenge", "CLOCK_SYNCED ",
    "Touch init:", "Touch sampler:", "LCD init:", "PSRAM TEST:",
    "Video wallpaper playing:",
    "AIR_HISTORY ", "AIR_HISTORY_STATE ", "RADAR_RANGE ", "ENVIRONMENT_VIEW ",
)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=1860)
    args = parser.parse_args()
    os.umask(0o077)
    args.output.mkdir(mode=0o700, parents=True, exist_ok=True)
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    connection.dtr = False
    connection.rts = False
    connection.port = args.port
    start = time.monotonic()
    boots = 0
    pending = bytearray()
    with (args.output / "runtime.jsonl").open("w") as records, \
         (args.output / "runtime.log").open("w") as log:
        def emit(line):
            row = dict(elapsed_s=round(time.monotonic()-start, 3),
                       utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), line=line)
            records.write(json.dumps(row, ensure_ascii=False) + "\n"); records.flush()
            log.write(f"{row['elapsed_s']:.3f} {line}\n"); log.flush()
        try:
            connection.open()
            emit("CAPTURE_OPENED; USB open may induce an expected initial reset")
            while time.monotonic()-start < args.seconds and not (args.output / "stop-capture").exists():
                pending.extend(connection.read(connection.in_waiting or 1))
                while b"\n" in pending:
                    raw, _, rest = pending.partition(b"\n"); pending = bytearray(rest)
                    line = raw.decode("utf-8", "replace").strip()
                    if line.startswith("BOOT_METRICS "): boots += 1
                    fault = any(word in line for word in
                                ("Guru Meditation", "Brownout detector", "watchdog got triggered", "FATAL:"))
                    if line.startswith(PREFIXES): emit(line)
                    if fault or boots > 1:
                        emit("CAPTURE_FAULT; disconnect power and inspect hardware")
                        (args.output / "hardware-fault.txt").write_text("unexpected boot or fault\n")
                        return 2
                if len(pending) > 8192:
                    emit("CAPTURE_LINE_OVERFLOW"); pending.clear()
            emit("CAPTURE_CLOSED")
        except (serial.SerialException, OSError) as error:
            emit("CAPTURE_IO_ERROR " + type(error).__name__)
            return 1
        finally:
            connection.close()
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
