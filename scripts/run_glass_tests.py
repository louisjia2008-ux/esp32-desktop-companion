#!/usr/bin/env python3
"""Render the real generated clock data under address/undefined sanitizers."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="glass-tests-") as directory:
        output = Path(directory) / "glass_tests"
        subprocess.run([
            *shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-O2",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Itests/touch/stubs", "-Iinclude", "-Isrc",
            "src/ui/glass_clock.cpp", "src/ui/glass_font.cpp", "src/ui/glass_font_data.cpp",
            "src/ui/text_layout.cpp", "tests/touch/test_glass.cpp", "-o", str(output),
        ], cwd=root, check=True)
        subprocess.run([str(output)], cwd=root, check=True)


if __name__ == "__main__":
    main()
