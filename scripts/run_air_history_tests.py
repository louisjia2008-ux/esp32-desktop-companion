#!/usr/bin/env python3
"""Offline real air-history core/SD adapter tests; never sensor readings."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    scenarios = [
        "codec", "calendar", "aggregate", "freshness", "clock_changes",
        "midnight", "queue_bound", "plot_gaps", "plot_duplicates",
        "roundtrip", "retention", "protected_file", "torn_tail",
        "corrupt_record", "truncated_header", "deferred_io", "write_retry",
        "read_retry", "allocation_fallback", "allocation_failure",
        "no_clock", "sd_unavailable", "record_limit", "reboot_minute",
        "readback_retry", "directory_retry", "rollback_collision", "torn31",
        "partial_write_retry", "date_request_during_load", "poll_gap",
    ]
    with tempfile.TemporaryDirectory(prefix="air-history-tests-") as directory:
        executable = Path(directory) / "air_history"
        command = [*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                   "-Wall", "-Wextra", "-Werror", "-pedantic",
                   f"-I{root / 'tests/history/stubs'}", f"-I{root / 'src'}",
                   str(root / "src/sensors/air_history.cpp"),
                   str(root / "src/storage/air_history.cpp"),
                   str(root / "tests/history/test_air_history.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        compiled = subprocess.run(command, cwd=root)
        if compiled.returncode:
            return compiled.returncode
        for scenario in scenarios:
            tested = subprocess.run([str(executable), scenario], cwd=root)
            if tested.returncode:
                return tested.returncode
    print(f"air history: {len(scenarios)}/{len(scenarios)} PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
