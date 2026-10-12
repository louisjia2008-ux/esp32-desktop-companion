#!/usr/bin/env python3
"""Reset an ESP32 through DTR/RTS and capture a bounded serial boot log."""

import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument(
        "--no-reset",
        action="store_true",
        help="observe the current serial stream without toggling DTR/RTS",
    )
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.1) as connection:
        if not args.no_reset:
            # This is the same hard-reset sequence esptool uses after upload.
            connection.dtr = False
            connection.rts = True
            time.sleep(0.1)
            connection.rts = False
        connection.reset_input_buffer()

        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            data = connection.read(connection.in_waiting or 1)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
