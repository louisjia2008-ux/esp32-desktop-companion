#!/usr/bin/env python3
"""Build and run the host-side WifiManager tests."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    source = repo_root / "src" / "network" / "wifi_manager.cpp"
    test_source = repo_root / "tests" / "wifi_manager" / "test_wifi_manager.cpp"
    stubs = repo_root / "tests" / "wifi_manager" / "stubs"

    for required_path in (source, test_source, stubs):
        if not required_path.exists():
            print(f"Missing required test input: {required_path}", file=sys.stderr)
            return 2

    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler:
        print("CXX must name a C++ compiler.", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="wifi-manager-tests-") as temp_dir:
        executable = Path(temp_dir) / "wifi_manager_tests"
        compile_command = [
            *compiler,
            "-std=c++17",
            f"-I{stubs}",
            f"-I{repo_root / 'src'}",
            f"-I{repo_root / 'src' / 'network'}",
            str(source),
            str(test_source),
            "-o",
            str(executable),
        ]
        compile_result = subprocess.run(compile_command, cwd=repo_root)
        if compile_result.returncode != 0:
            return compile_result.returncode

        test_result = subprocess.run([str(executable)], cwd=repo_root)
        return test_result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
