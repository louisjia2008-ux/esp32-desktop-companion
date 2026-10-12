#!/usr/bin/env python3
"""Exercise the production whole-line drop policy without a USB device."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="runtime-log-tests-") as directory:
    binary = Path(directory) / "log_tests"
    subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++11",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-DSENSOR_SERVICE_ENABLED=1", "-Itests/logging/stubs", "-Isrc",
                    "tests/logging/test_runtime_log.cpp", "-o", str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], cwd=root, check=True)
