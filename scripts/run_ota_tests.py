#!/usr/bin/env python3
"""Run the firmware OTA receiver with a controlled flash backend on macOS."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='esp32-ota-tests-') as directory:
    binary=Path(directory)/'ota-tests'
    command=[*shlex.split(os.environ.get('CXX','c++')),'-std=c++17','-Wall','-Wextra',
             '-Wno-deprecated-declarations','-fsanitize=address,undefined',
             '-Itests/ota/stubs','-Itests/touch/stubs','-Iinclude','-Isrc',
             'src/storage/ota_receiver.cpp','tests/ota/test_ota.cpp','-o',str(binary)]
    subprocess.run(command,cwd=root,check=True)
    subprocess.run([str(binary)],check=True)
