#!/usr/bin/env python3
"""Compile the production adapter with actual pinned WebSockets 2.6.1 sources.

The host WiFiClient stub deliberately uses the ESP32 2.0.17 seconds-based
setTimeout API. This is offline software evidence, not a device/network test.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--websockets-src", type=Path,
                        default=root / ".pio/libdeps/deskpet_sensors/WebSockets/src")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    library = args.websockets_src.resolve()
    version = library / "WebSocketsVersion.h"
    if not version.exists() or '#define WEBSOCKETS_VERSION "2.6.1"' not in version.read_text():
        raise SystemExit("Provide the actual pinned WebSockets 2.6.1 src directory via --websockets-src")
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    c_compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler or not c_compiler:
        raise SystemExit("CXX and CC must name compilers")
    sanitizers = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
                  if args.sanitize else [])
    for name in ("WebSockets.cpp", "WebSocketsClient.cpp", "WebSocketsClient.h"):
        digest = hashlib.sha256((library / name).read_bytes()).hexdigest()
        print(f"SOURCE {name} SHA256={digest}", flush=True)
    stubs = root / "tests/network_recovery/stubs"
    with tempfile.TemporaryDirectory(prefix="network-recovery-tests-") as directory:
        output = Path(directory)
        sha_object = output / "sha1.o"
        sha_wrapper = output / "sha_wrapper.o"
        base64_object = output / "base64.o"
        commands = [
            [*c_compiler, "-c", str(library / "libsha1/libsha1.c"),
             "-o", str(sha_object), *sanitizers],
            [*c_compiler, "-c", str(library / "libb64/cencode.c"),
             "-o", str(base64_object), *sanitizers],
            [*compiler, "-std=c++17", f"-I{library}", "-c",
             str(root / "tests/network_recovery/sha_stub.cpp"),
             "-o", str(sha_wrapper), *sanitizers],
            [*compiler, "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter",
             "-DESP32", "-DWEBSOCKETS_TCP_TIMEOUT=1500", "-DNODEBUG_WEBSOCKETS",
             f"-I{stubs}", f"-I{library}", f"-I{root / 'src'}",
             str(library / "WebSockets.cpp"), str(library / "WebSocketsClient.cpp"),
             str(root / "src/network/recovering_websocket_client.cpp"),
             str(root / "tests/network_recovery/test_recovering_websocket.cpp"),
             str(sha_object), str(sha_wrapper), str(base64_object),
             "-o", str(output / "network_recovery"),
             *sanitizers],
            [str(output / "network_recovery")],
        ]
        for command in commands:
            result = subprocess.run(command, cwd=root)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
