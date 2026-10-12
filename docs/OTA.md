# Bridge wireless firmware updates

An initial USB installation is required. Subsequent updates use the existing
paired WebSocket connection and the physical device's confirmation card.
The partition table has two 0x640000-byte (6.25 MiB) application slots.

## Send an update

Build with `.venv/bin/pio run`, then keep the Bridge running and use its local
browser editor's **无线升级** section. Select `firmware.bin`, send it, and hold
**安装** on the ESP32 for 1.5 seconds. **取消** rejects it. The offer expires
after two minutes. Keep power connected during installation.

The CLI sends the same request to the running Bridge:

```sh
.venv/bin/python mac_bridge/bridge.py ota
.venv/bin/python mac_bridge/bridge.py ota /absolute/path/firmware.bin
```

The default file is `.pio/build/deskpet_sensors/firmware.bin`, matching the
default PlatformIO environment. To offer the no-sensor fallback, build it with
`.venv/bin/pio run -e jc3248w535` and explicitly pass
`.pio/build/jc3248w535/firmware.bin` as the firmware argument. A merged flash image
or a bootloader image is not accepted. Before publishing a different release,
change `include/firmware_version.h` and build; never binary-patch the version.

## Validation and transport

`POST /api/ota` accepts bounded `application/octet-stream` bytes, never a
server-side path. The editor remains bound to loopback and rejects mismatched
Host/Origin and ambiguous transfer framing. Exactly one paired, ready device
with the `ota` capability must be connected.

The Bridge checks ESP image magic, ESP32-S3 chip ID, segment boundaries, app
descriptor, XOR checksum, appended image SHA-256, exact file length, partition
size and one `DEVCOMPANION_FW=x.y.z` marker. `DEVCOMPANION_BUILD` carries the
application compilation time; the framework's prebuilt descriptor time is
only a compatibility fallback.

An `ota_offer` binds transfer ID, size, full-file SHA-256, version, build time
and a fresh device connection nonce using HMAC-SHA256 and the pairing key.
Only the physical confirmation path calls `Update.begin`. This is a paired
local-network update mechanism, not vendor secure boot or encrypted transport.

Each binary `DWO1` packet has a 24-byte little-endian header:

| Offset | Value |
| ---: | --- |
| 0 | `DWO1` magic |
| 4 | uint64 transfer ID |
| 12 | uint32 byte offset |
| 16 | uint32 payload length, up to 12 KiB |
| 20 | uint32 CRC32 of payload |
| 24 | payload bytes |

The Bridge sends 1 KiB payloads, below the receiver's 12 KiB limit, to keep
individual frames small on LANs with intermittent latency. It waits for each
ACK and retries a lost ACK at most twice. An exact
duplicate chunk is acknowledged without another flash write. Invalid CRC,
offset, size, image header, final SHA, a disconnected transfer or a stalled
transfer aborts the inactive slot. A successful commit requires the exact
advertised byte count and `Update.end(false)`, then delays reboot by 1.5 s.
SD transfer and wallpaper decoding pause while the update is active.

## Boot confirmation and rollback

Arduino's `verifyRollbackLater()` is overridden to defer automatic acceptance.
The new app confirms only after display and touch initialization, a connected
Wi-Fi station, and 30 seconds of stable operation. Bridge connectivity is not
a health requirement. If the app crashes/reboots before confirmation, the
enabled bootloader rollback mechanism selects the previous valid image.
An unconfirmed app that cannot satisfy health checks within three minutes
requests rollback itself.

The Bridge treats transfer completion as **rebooting**. An authenticated
`ota_boot` must identify the new version and the exact embedded image digest:
`pending` means **validating**, `valid` means **complete**, and an unknown
partition state remains **unconfirmed**. A verified return to the previous
version and previous image digest is **rolled_back**. Silence is never success.

OTA job status is held in Bridge memory. Keep the Bridge running through the
update; after a Bridge restart its old transfer result cannot be reconstructed
from a version number alone. A missing confirmation remains unconfirmed and
blocks another offer until the state is resolved or the Bridge is restarted
after inspecting the device. A local USB cable remains useful for recovery.

## Verification

```sh
.venv/bin/python scripts/run_ota_tests.py
PYTHONPATH=mac_bridge .venv/bin/python -m unittest mac_bridge.tests.test_ota -v
```

Host tests exercise physical-consent gating, chunk retries/duplicates, CRC,
ordering, image/hash rejection, disconnects, timeouts, partition bounds,
same-origin HTTP, and an actual loopback WebSocket transfer with boot identity
checks. Real flash writes, actual long-press consent, bootloader rollback and
visible progress require the device. Keep fault-injection binaries separate
from normal firmware and only run them with a known recovery path.
