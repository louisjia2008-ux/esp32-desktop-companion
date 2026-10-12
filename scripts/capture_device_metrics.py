#!/usr/bin/env python3
"""Capture selected ESP32 metrics and reset diagnostics.

DTR/RTS are deasserted before opening, but OS/driver transitions can still reset
the target. Open this capture before OTA and keep the connection stable.
"""
import argparse
import re
import time
from pathlib import Path

import serial

METRIC_PREFIXES = (
    'METRICS ', 'EVENT_METRICS ', 'TOUCH_METRICS ', 'TOUCH ', 'PAGE ', 'WebSocket ', 'OTA ',
    'SD ', 'BOOT ', 'FW ', 'HUB_', 'SETUP_EXIT', 'CLOCK_VISIBLE=',
    'BOOT_METRICS ', 'BOOT_HEALTH ', 'DEVCOMPANION_FW=', 'DEVCOMPANION_BUILD=',
)
# Keep reset codes, panic categories and addresses, without arbitrary raw output
# such as assertion messages, source paths, task names or serial input echoes.
RESET_DIAGNOSTIC = re.compile(
    r'(?:ESP-ROM:[A-Za-z0-9_-]+|'
    r'rst:0x[0-9A-Fa-f]+ \([A-Z0-9_]+\),boot:0x[0-9A-Fa-f]+ \([A-Z0-9_]+\)|'
    r'Saved PC:0x[0-9A-Fa-f]+|'
    r"Guru Meditation Error: Core\s+[01]\s+panic'ed "
    r'\([A-Za-z0-9 /_-]{1,80}\)\.?(?: Exception was unhandled\.)?|'
    r'Backtrace:(?:\s+0x[0-9A-Fa-f]+:0x[0-9A-Fa-f]+)+(?:\s+\|<-CORRUPTED)?|'
    r'ELF file SHA256:\s*[0-9A-Fa-f]{8,64}(?:\.\.\.)?|'
    r'Brownout detector was triggered|Rebooting\.\.\.)'
)
WIFI_ADDRESS = re.compile(
    r'Wi-Fi connected IP=(\d{1,3}(?:\.\d{1,3}){3}) '
    r'gateway=(\d{1,3}(?:\.\d{1,3}){3}) '
    r'bssid=[0-9A-Fa-f:]{17} channel=(\d{1,2}) rssi=(-?\d{1,3})'
)
WIFI_DIAGNOSTIC = re.compile(
    r'(?:Wi-Fi connection attempt \d+|'
    r'Wi-Fi station disconnected reason=\d+ \([A-Z0-9_]+\)|'
    r'Wi-Fi disconnected; retry scheduled|'
    r'Wi-Fi startup connection timed out; entering setup mode)'
)


def selected_line(line):
    network = WIFI_ADDRESS.fullmatch(line)
    if network:
        ip, gateway, channel, rssi = network.groups()
        return f'WIFI_NETWORK ip={ip} gateway={gateway} channel={channel} rssi={rssi}'
    if (line.startswith(METRIC_PREFIXES) or RESET_DIAGNOSTIC.fullmatch(line)
            or WIFI_DIAGNOSTIC.fullmatch(line)):
        return line
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='serial port selected from your connected devices')
    parser.add_argument('--seconds', type=int, default=40)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument(
        '--reconnect', action='store_true',
        help='retry after serial I/O errors; each reopen may reset the target '
             '(default: log the error type and stop)',
    )
    args = parser.parse_args()
    deadline = time.monotonic() + args.seconds
    connection = None
    exit_code = 0
    with args.output.open('w') as log:
        def emit(line):
            log.write(line + '\n')
            log.flush()
            print(line, flush=True)

        def io_error(error, operation, action):
            # Exception messages can contain private paths or device details.
            kind = 'SerialException' if isinstance(error, serial.SerialException) else 'OSError'
            emit(f'CAPTURE_IO_ERROR type={kind} operation={operation} action={action}')

        def close_connection():
            nonlocal connection
            if connection is None:
                return True
            try:
                connection.close()
            except (serial.SerialException, OSError) as error:
                io_error(error, 'close', 'stop')
                return False
            finally:
                connection = None
            return True

        try:
            while time.monotonic() < deadline:
                operation = 'read'
                try:
                    if connection is None:
                        operation = 'open'
                        emit('CAPTURE_OPEN_ATTEMPT')
                        connection = serial.Serial(port=None, baudrate=115200, timeout=.2)
                        connection.dtr = False
                        connection.rts = False
                        connection.port = args.port
                        connection.open()
                        emit('CAPTURE_OPENED')
                    operation = 'read'
                    raw = connection.readline()
                except (serial.SerialException, OSError) as error:
                    io_error(error, operation, 'retry' if args.reconnect else 'stop')
                    closed = close_connection()
                    if not args.reconnect or not closed:
                        exit_code = 1
                        break
                    time.sleep(.5)
                    continue
                if raw:
                    line = raw.decode('utf-8', 'replace').strip()
                    selected = selected_line(line)
                    if selected is not None:
                        emit(selected)
        finally:
            if not close_connection():
                exit_code = 1
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
