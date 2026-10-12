#!/usr/bin/env python3
"""Run offline sensor parser/policy tests, not physical sensor evidence."""

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
    with tempfile.TemporaryDirectory(prefix="sensor-tests-") as directory:
        suites = {
            "ld2410c_parser": [
                "src/sensors/ld2410c_parser.cpp",
                "tests/sensors/test_ld2410c_parser.cpp",
            ],
            "sensor_policy": [
                "src/sensors/sensor_policy.cpp",
                "src/ui/backlight_policy.cpp",
                "tests/sensors/test_sensor_policy.cpp",
            ],
        }
        for name, sources in suites.items():
            executable = Path(directory) / name
            command = [
                *compiler,
                "-std=c++11",
                "-Wall", "-Wextra", "-Werror", "-pedantic",
                f"-I{root / 'src'}",
                *(str(root / source) for source in sources),
                "-o", str(executable),
            ]
            if args.sanitize:
                command.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"])
            compiled = subprocess.run(command, cwd=root)
            if compiled.returncode != 0:
                return compiled.returncode
            tested = subprocess.run([str(executable)], cwd=root)
            if tested.returncode != 0:
                return tested.returncode
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
