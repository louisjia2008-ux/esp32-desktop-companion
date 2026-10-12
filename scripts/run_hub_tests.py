#!/usr/bin/env python3
"""Compile the actual desktop hub parser, model, UI and QR code with sanitizers."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='esp32-hub-tests-') as d:
 binary=Path(d)/'hub-tests'
 sources=['src/hub/hub_model.cpp','src/hub/hub_protocol.cpp','src/hub/hub_ui.cpp','src/hub/qr/qrcodegen.c',
          'src/ui/ui_manager.cpp','src/ui/event_system.cpp','src/ui/text_layout.cpp','src/ui/backlight_policy.cpp',
          'src/ui/glass_clock.cpp','src/ui/glass_surface.cpp','src/ui/glass_font.cpp','tests/touch/glass_font_stub.cpp','src/protocol/protocol.cpp','tests/hub/test_hub.cpp']
 command=[*shlex.split(os.environ.get('CXX','c++')),'-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined',
          *[f'-I{root/p}' for p in ['tests/touch/stubs','include','src','.pio/libdeps/jc3248w535/ArduinoJson/src']],
          *[str(root/p) for p in sources],'-o',str(binary)]
 code=subprocess.run(command,cwd=root).returncode
 if not code:code=subprocess.run([str(binary)],cwd=root).returncode
 raise SystemExit(code)
