#!/usr/bin/env python3
"""Exercise the real service worker code with offline bus/task stubs.

These tests are software evidence only, never actual sensor measurements.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="enable ASan and UBSan")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("CXX must name a C++ compiler")
    scenarios = [
        "happy", "allocation_independence", "uart_ownership", "i2c_ownership",
        "address_recovery", "communication_recovery", "recovery_wrap",
        "sample_timeout", "error_classification", "variant_recovery",
        "invalid_measurements", "measurement_failure",
        "radar_stale_recovery", "freshness",
    ]
    with tempfile.TemporaryDirectory(prefix="sensor-service-tests-") as directory:
        executable = Path(directory) / "sensor_service"
        command = [
            *compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-DSENSOR_SERVICE_ENABLED=1",
            f"-I{root / 'tests/sensor_service/stubs'}", f"-I{root / 'src'}",
            str(root / "src/sensors/sensor_service.cpp"),
            str(root / "src/sensors/ld2410c_parser.cpp"),
            str(root / "tests/sensor_service/test_sensor_service.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        compiled = subprocess.run(command, cwd=root)
        if compiled.returncode:
            return compiled.returncode
        for scenario in scenarios:
            tested = subprocess.run([str(executable), scenario], cwd=root)
            if tested.returncode:
                return tested.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
