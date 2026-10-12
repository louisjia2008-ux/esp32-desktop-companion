#!/usr/bin/env python3
"""Exercise the actual SD cache state machine using a temporary host filesystem."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sd-wallpaper-tests-") as temporary:
    directory = Path(temporary)
    card = directory / "card"
    card.mkdir()
    binary = directory / "sd-tests"
    command = [*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra",
               "-fsanitize=address,undefined", f"-I{root/'tests/storage/stubs'}",
               f"-I{root/'tests/touch/stubs'}", f"-I{root/'include'}", f"-I{root/'src'}",
               str(root/'src/storage/sd_card.cpp'), str(root/'src/storage/sd_wallpaper.cpp'),
               str(root/'tests/storage/test_sd_wallpaper.cpp'), "-o", str(binary)]
    result = subprocess.run(command, cwd=root)
    if result.returncode: raise SystemExit(result.returncode)
    raise SystemExit(subprocess.run([str(binary), str(card)], cwd=root).returncode)
