#!/usr/bin/env python3
"""Compile the actual firmware event/UI/JSON code on the host."""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="event-tests-") as temporary:
        binary = Path(temporary) / "events"
        command = [*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                   "-Wall", "-Wextra", "-fsanitize=address,undefined",
                   f"-I{root / 'tests/touch/stubs'}", f"-I{root / 'include'}",
                   f"-I{root / 'src'}", f"-I{root / '.pio/libdeps/jc3248w535/ArduinoJson/src'}",
                   str(root / "src/ui/event_system.cpp"), str(root / "src/ui/ui_manager.cpp"),
                   str(root / "src/ui/text_layout.cpp"), str(root / "src/ui/backlight_policy.cpp"),
                   str(root / "src/ui/glass_clock.cpp"), str(root / "src/ui/glass_surface.cpp"), str(root / "src/ui/glass_font.cpp"),
                   str(root / "tests/touch/glass_font_stub.cpp"),
                   str(root / "src/protocol/protocol.cpp"), str(root / "tests/touch/test_events.cpp"),
                   "-o", str(binary)]
        command[1:1] = [str(root / "src/hub/hub_model.cpp"),str(root / "src/hub/hub_ui.cpp"),str(root / "src/hub/qr/qrcodegen.c")]
        command[1:1] = [str(root / "src/hub/hub_protocol.cpp")]
        compiled = subprocess.run(command, cwd=root)
        if compiled.returncode:
            return compiled.returncode
        tested = subprocess.run([str(binary)], cwd=root)
        if tested.returncode:
            return tested.returncode
        # The event card also fits the native portrait dimensions, without
        # changing the existing product's landscape driver or touch transform.
        portrait = Path(temporary) / "events-portrait"
        command.insert(3, f"-I{root / 'tests/events/portrait'}")
        command[-1] = str(portrait)
        compiled = subprocess.run(command, cwd=root)
        if compiled.returncode:
            return compiled.returncode
        return subprocess.run([str(portrait)], cwd=root).returncode


if __name__ == "__main__":
    raise SystemExit(main())
