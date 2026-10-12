from __future__ import annotations

import asyncio
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zlib

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

from bridge import Bridge, ProtocolError, parse_device_message
from sd_transfer import SdTransfer
from test_bridge import write_wallpaper_record
from wallpaper_cache import (CachePackageError, CHUNK_HEADER, HEADER,
                             build_package, chunk_packet, inspect_package)
from wallpaper_library import WallpaperLibrary


ID = "0123456789abcdef"
JPEG = b"\xff\xd8test\xff\xd9"


class PackageTests(unittest.TestCase):
    def test_ten_minute_packages_at_each_frame_rate_and_overflow(self):
        with tempfile.TemporaryDirectory() as directory:
            for fps in (6, 8, 10, 12):
                count = fps * 600
                path = Path(directory) / f"{fps}.dwc"
                record = SimpleNamespace(wallpaper_id=ID, frame_count=count, fps=fps)
                package = build_package(record, lambda _: JPEG, path)
                self.assertEqual(inspect_package(path, ID).frame_count, count)
                self.assertEqual(package.fps, fps)
                over_limit = SimpleNamespace(wallpaper_id=ID, frame_count=count+1, fps=fps)
                with self.assertRaises(CachePackageError):
                    build_package(over_limit, lambda _: self.fail("oversize package reads frames"), path)
                self.assertEqual(inspect_package(path, ID), package)
                # A valid CRC cannot make an over-duration header acceptable.
                invalid = bytearray(path.read_bytes())
                struct.pack_into("<I", invalid, 16, count+1)
                struct.pack_into("<I", invalid, 36, zlib.crc32(invalid[:36]))
                path.write_bytes(invalid)
                with self.assertRaises(CachePackageError): inspect_package(path, ID)

    def test_package_roundtrip_and_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.dwc"
            record = SimpleNamespace(wallpaper_id=ID, frame_count=2, fps=10)
            package = build_package(record, lambda _: JPEG, path)
            self.assertEqual(inspect_package(path, ID), package)
            self.assertEqual(package.size, 40 + 2 * (4 + len(JPEG)))
            self.assertEqual(build_package(record, lambda _: self.fail("cached package rebuilt"), path), package)
            self.assertEqual(package.crc32, zlib.crc32(path.read_bytes()))

    def test_corruption_truncation_and_trailing_bytes_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.dwc"
            record = SimpleNamespace(wallpaper_id=ID, frame_count=2, fps=10)
            build_package(record, lambda _: JPEG, path)
            original = path.read_bytes()
            cases = [original[:-1], original + b"junk"]
            corrupt = bytearray(original); corrupt[-5] ^= 1; cases.append(corrupt)
            corrupt = bytearray(original); corrupt[8] ^= 1; cases.append(corrupt)
            for data in cases:
                path.write_bytes(data)
                with self.assertRaises(CachePackageError): inspect_package(path, ID)
            repaired = build_package(record, lambda _: JPEG, path)
            self.assertEqual(repaired.crc32, zlib.crc32(original))

    def test_invalid_frames_and_chunk_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.dwc"
            record = SimpleNamespace(wallpaper_id=ID, frame_count=1, fps=10)
            for jpeg in (b"bad", b"\xff\xd8" + b"x" * 15000 + b"\xff\xd9"):
                with self.assertRaises(CachePackageError): build_package(record, lambda _: jpeg, path)
            for token, offset, data in (("bad", 0, b"x"), (ID, -1, b"x"), (ID, 0, b""), (ID, True, b"x")):
                with self.assertRaises(CachePackageError): chunk_packet(token, offset, data)
            packet = chunk_packet(ID, 15, JPEG)
            self.assertEqual(CHUNK_HEADER.unpack(packet[:20]), (b"DWS1", int(ID,16), 15, zlib.crc32(JPEG)))

    def test_sync_ack_validation(self):
        message = {"v": 1, "type": "wallpaper_sync_ack", "transfer_id": ID,
                   "wallpaper_id": ID, "state": "ready", "offset": 0,
                   "bytes": 48, "reason": "", "sd_capacity_bytes": 1004535808}
        self.assertEqual(parse_device_message(json.dumps(message)), message)
        for changes in ({"state":"unknown"}, {"offset":49}, {"bytes":True},
                        {"transfer_id":"../../file"}, {"command":"test"},
                        {"sd_capacity_bytes":-1}):
            with self.assertRaises(ProtocolError): parse_device_message(json.dumps({**message, **changes}))


class TransferTests(unittest.IsolatedAsyncioTestCase):
    async def test_long_verification_survives_old_180_second_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            package = build_package(SimpleNamespace(wallpaper_id=ID, frame_count=7200, fps=12),
                                    lambda _: JPEG, Path(directory)/"long.dwc")
            clock = [0.0]
            tasks = []
            def ack(state, offset):
                transfer.feed({"transfer_id":transfer.transfer_id,"wallpaper_id":ID,
                               "state":state,"offset":offset,"bytes":package.size,
                               "reason":"","sd_capacity_bytes":1004535808})
            async def finish_later():
                await asyncio.sleep(.01)
                clock[0] = 181.0
                ack("verifying", package.size)
                await asyncio.sleep(.01)
                ack("complete", package.size)
            async def send(packet):
                if isinstance(packet, dict):
                    if packet["op"]=="begin": ack("ready",0)
                    elif packet["op"]=="commit":
                        ack("verifying",package.size)
                        tasks.append(asyncio.create_task(finish_later()))
                else:
                    offset=CHUNK_HEADER.unpack(packet[:20])[2]
                    ack("progress",offset+len(packet)-20)
            transfer = SdTransfer(ID, lambda:package, send)
            with mock.patch("sd_transfer.time", SimpleNamespace(monotonic=lambda:clock[0])):
                await asyncio.wait_for(transfer.run(), 2)
            await asyncio.gather(*tasks)
            self.assertEqual(transfer.state, "complete")
            self.assertEqual(transfer.offset, package.size)

    async def exercise(self, *, cached=False, drop_first=False, stop=False):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.dwc"
            package = build_package(SimpleNamespace(wallpaper_id=ID, frame_count=2, fps=10), lambda _: JPEG, path)
            received = bytearray()
            chunks = []
            def ack(state, offset):
                transfer.feed({"transfer_id": transfer.transfer_id, "wallpaper_id": ID,
                               "state":state, "offset":offset, "bytes":package.size,
                               "reason":"", "sd_capacity_bytes":1004535808})
            async def send(packet):
                if isinstance(packet, dict):
                    if packet["op"] == "begin": ack("cached" if cached else "ready", package.size if cached else 0)
                    elif packet["op"] == "commit":
                        self.assertEqual(bytes(received), path.read_bytes())
                        ack("complete", len(received))
                else:
                    chunks.append(packet)
                    if not received: received.extend(packet[20:])
                    if stop: transfer.stop()
                    elif not drop_first or len(chunks) > 1: ack("progress", len(received))
            transfer = SdTransfer(ID, lambda: package, send, ack_timeout=0.02)
            await asyncio.wait_for(transfer.run(), timeout=2)
            return transfer, chunks

    async def test_complete_upload(self):
        transfer, chunks = await self.exercise()
        self.assertEqual(transfer.state, "complete")
        self.assertEqual(len(chunks), 1)

    async def test_cached_reconnect_sends_no_binary_frames(self):
        transfer, chunks = await self.exercise(cached=True)
        self.assertEqual(transfer.state, "cached")
        self.assertEqual(chunks, [])

    async def test_lost_chunk_ack_retries_identical_bytes(self):
        transfer, chunks = await self.exercise(drop_first=True)
        self.assertEqual(transfer.state, "complete")
        self.assertEqual(len(chunks), 2)
        self.assertEqual(chunks[0], chunks[1])

    async def test_selection_change_aborts_transfer(self):
        transfer, _ = await self.exercise(stop=True)
        self.assertEqual(transfer.state, "aborted")


class SocketTests(unittest.IsolatedAsyncioTestCase):
    async def test_real_websocket_sd_upload_and_legacy_stream_isolation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = write_wallpaper_record(root, ID, "test.mp4")
            library = WallpaperLibrary(root); library.set_active(ID)
            bridge = Bridge(library)
            await bridge.start()
            try:
                async with serve(bridge.handle_client, "127.0.0.1", 0, subprotocols=["arduino"]) as server:
                    port = server.sockets[0].getsockname()[1]
                    async with connect(f"ws://127.0.0.1:{port}", subprotocols=["arduino"]) as device:
                        await device.send(json.dumps({"v":1,"type":"hello","device":"sd-test",
                                                      "firmware":"0.5.0","width":480,"height":320,
                                                      "capabilities":["sd_wallpaper"]}))
                        control = None
                        data = bytearray()
                        while True:
                            raw = await asyncio.wait_for(device.recv(), 3)
                            if isinstance(raw, str):
                                packet = json.loads(raw)
                                self.assertEqual(packet["type"], "wallpaper_sync")
                                if packet["op"] == "begin":
                                    control = packet; state = "ready"
                                elif packet["op"] == "commit":
                                    self.assertEqual(len(data), control["bytes"])
                                    self.assertEqual(zlib.crc32(data), control["crc32"])
                                    state = "complete"
                                else: self.fail("unexpected sync operation")
                            else:
                                magic, token, offset, crc = CHUNK_HEADER.unpack(raw[:20])
                                self.assertEqual(magic, b"DWS1")
                                self.assertEqual(token, int(control["transfer_id"],16))
                                self.assertEqual(offset,len(data))
                                self.assertEqual(crc,zlib.crc32(raw[20:]))
                                data.extend(raw[20:]); state = "progress"
                            await device.send(json.dumps({"v":1,"type":"wallpaper_sync_ack",
                                "transfer_id":control["transfer_id"],"wallpaper_id":ID,
                                "state":state,"offset":len(data),"bytes":control["bytes"],
                                "reason":"","sd_capacity_bytes":1004535808}))
                            if state == "complete": break
                        await device.send(json.dumps({"v":1,"type":"wallpaper_status",
                                                       "wallpaper_id":ID,"state":"playing","sequence":0}))
                        await asyncio.sleep(0.05)
                        status = await bridge.status_snapshot()
                        self.assertEqual(status["playback_mode"],"sd_local")
                        self.assertEqual(status["wallpaper_state"],"playing")
                        self.assertEqual(status["streamed_frame_packets"],0)
                        async with connect(f"ws://127.0.0.1:{port}", subprotocols=["arduino"]) as legacy:
                            await legacy.send(json.dumps({"v":1,"type":"hello","device":"legacy-test",
                                                          "firmware":"0.4.0","width":480,"height":320,
                                                          "capabilities":["video_wallpaper"]}))
                            self.assertEqual(json.loads(await legacy.recv())["type"],"video_wallpaper")
                            self.assertTrue((await legacy.recv()).startswith(b"DWP1"))
                            with self.assertRaises(asyncio.TimeoutError):
                                await asyncio.wait_for(device.recv(),0.2)
                        await bridge.delete_wallpaper(ID)
                        clear = json.loads(await asyncio.wait_for(device.recv(),2))
                        self.assertEqual(clear["op"],"clear")
            finally:
                await bridge.shutdown()
