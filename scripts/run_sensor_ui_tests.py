#!/usr/bin/env python3
"""Compile the production environment UI with real navigation and event code."""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("CXX must name a C++ compiler")
    sources = [
        "src/ui/ui_manager.cpp", "src/ui/event_system.cpp",
        "src/ui/text_layout.cpp", "src/ui/backlight_policy.cpp",
        "src/ui/glass_clock.cpp", "src/ui/glass_surface.cpp",
        "src/ui/glass_font.cpp", "tests/touch/glass_font_stub.cpp",
        "src/hub/hub_model.cpp", "src/hub/hub_ui.cpp", "src/hub/qr/qrcodegen.c",
        "tests/sensors/test_environment_ui.cpp",
    ]
    with tempfile.TemporaryDirectory(prefix="sensor-ui-tests-") as directory:
        executable = Path(directory) / "environment_ui_tests"
        command = [
            *compiler, "-std=c++17", "-DSENSOR_SERVICE_ENABLED=1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            f"-I{root / 'tests/touch/stubs'}", f"-I{root / 'include'}",
            f"-I{root / 'src'}", *[str(root / item) for item in sources],
            "-o", str(executable),
        ]
        compiled = subprocess.run(command, cwd=root)
        if compiled.returncode:
            return compiled.returncode
        return subprocess.run([str(executable)], cwd=root).returncode


if __name__ == "__main__":
    raise SystemExit(main())
