#!/usr/bin/env python3
"""Compile and test the actual UI touch logic and background sampler."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("CXX must name a C++ compiler")
    with tempfile.TemporaryDirectory(prefix="touch-tests-") as directory:
        executable = Path(directory) / "touch_tests"
        command = [
            *compiler,
            "-std=c++17",
            f"-I{root / 'tests' / 'touch' / 'stubs'}",
            f"-I{root / 'include'}",
            f"-I{root / 'src'}",
            str(root / "src" / "ui" / "ui_manager.cpp"),
            str(root / "src" / "ui" / "event_system.cpp"),
            str(root / "src" / "ui" / "text_layout.cpp"),
            str(root / "src" / "ui" / "backlight_policy.cpp"),
            str(root / "src" / "ui" / "glass_clock.cpp"),
            str(root / "src" / "ui" / "glass_surface.cpp"),
            str(root / "src" / "ui" / "glass_font.cpp"),
            str(root / "tests" / "touch" / "glass_font_stub.cpp"),
            str(root / "src" / "touch" / "touch_sampler.cpp"),
            str(root / "src" / "touch" / "touch_transform.cpp"),
            str(root / "tests" / "touch" / "test_touch.cpp"),
            "-o",
            str(executable),
        ]
        command[1:1] = [str(root / "src/hub/hub_model.cpp"),str(root / "src/hub/hub_ui.cpp"),str(root / "src/hub/qr/qrcodegen.c")]
        compiled = subprocess.run(command, cwd=root)
        if compiled.returncode != 0:
            return compiled.returncode
        tested = subprocess.run([str(executable)], cwd=root)
        if tested.returncode != 0:
            return tested.returncode
        driver_executable = Path(directory) / "touch_driver_tests"
        driver_command = [
            *compiler,
            "-std=c++17",
            f"-I{root / 'tests' / 'touch' / 'stubs'}",
            f"-I{root / 'include'}",
            str(root / "src" / "touch_driver.cpp"),
            str(root / "tests" / "touch" / "test_touch_driver.cpp"),
            "-o",
            str(driver_executable),
        ]
        compiled_driver = subprocess.run(driver_command, cwd=root)
        if compiled_driver.returncode != 0:
            return compiled_driver.returncode
        return subprocess.run([str(driver_executable)], cwd=root).returncode


if __name__ == "__main__":
    raise SystemExit(main())
