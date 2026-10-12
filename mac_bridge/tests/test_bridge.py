from __future__ import annotations

import asyncio
import ctypes
from dataclasses import asdict
import json
from pathlib import Path
import tempfile
import sys
from typing import Any
import unittest
from unittest import mock

from bridge import (
    Bridge,
    FRAME_HEADER,
    FRAME_HEADER_BYTES,
    FRAME_MAGIC,
    HOST_STATUS_CAPABILITY,
    MAX_PACKET_BYTES,
    HostTelemetrySampler,
    ProtocolError,
    action_required_message,
    build_video_frame,
    dismiss_message,
    encode_message,
    host_status_message,
    parse_device_message,
    video_control_message,
    _darwin_host_statistics,
    _darwin_libsystem,
)
from simulate_device import validate_host_status, validate_video_frame
from wallpaper_library import (
    MAX_FRAME_BYTES,
    WallpaperLibrary,
    WallpaperNotFoundError,
    WallpaperRecord,
    WallpaperStorageError,
)


def write_wallpaper_record(
    root: Path, wallpaper_id: str, name: str
) -> WallpaperRecord:
    record = WallpaperRecord(
        wallpaper_id=wallpaper_id,
        name=name,
        created_at="2026-09-30T00:00:00+00:00",
        duration_seconds=1.0,
        source_width=640,
        source_height=360,
        fps=10,
        fit="cover",
        frame_count=1,
        frame_bytes=8,
        source_bytes=8,
        source_media_type="video/mp4",
        jpeg_quality=9,
    )
    directory = root / wallpaper_id
    (directory / "frames").mkdir(parents=True)
    (directory / "source.bin").write_bytes(b"source")
    (directory / "thumbnail.jpg").write_bytes(b"thumb")
    (directory / "frames" / "000001.jpg").write_bytes(b"\xff\xd8xx\xff\xd9")
    (directory / "metadata.json").write_text(
        json.dumps(asdict(record)), encoding="utf-8"
    )
    return record


class FakeConnection:
    def __init__(self) -> None:
        self.sent: list[str | bytes] = []

    async def send(self, payload: str | bytes) -> None:
        self.sent.append(payload)


class BlockingConnection(FakeConnection):
    def __init__(self) -> None:
        super().__init__()
        self.send_entered = asyncio.Event()
        self.release_send = asyncio.Event()
        self.send_cancelled = False
        self.close_calls: list[tuple[int, str]] = []

    async def send(self, payload: str | bytes) -> None:
        self.send_entered.set()
        try:
            await self.release_send.wait()
        except asyncio.CancelledError:
            self.send_cancelled = True
            raise
        self.sent.append(payload)

    async def close(self, code: int = 1000, reason: str = "") -> None:
        self.close_calls.append((code, reason))
        self.release_send.set()


class FakeHostSampler:
    def __init__(self, samples: list[dict[str, Any] | Exception]) -> None:
        self.samples = iter(samples)

    def sample(self) -> dict[str, Any]:
        sample = next(self.samples)
        if isinstance(sample, Exception):
            raise sample
        return sample


class ProtocolTests(unittest.TestCase):
    def test_parses_hello(self) -> None:
        message = {
            "v": 1,
            "type": "hello",
            "device": "esp32-companion",
            "width": 480,
            "height": 320,
            "firmware": "0.1.0",
        }
        self.assertEqual(parse_device_message(json.dumps(message)), message)

    def test_parses_bounded_unique_capabilities(self) -> None:
        message = {
            "v": 1,
            "type": "hello",
            "device": "esp32-companion",
            "width": 480,
            "height": 320,
            "firmware": "0.3.0",
            "capabilities": ["video_wallpaper", "host_status"],
        }
        self.assertEqual(parse_device_message(json.dumps(message)), message)

        invalid_capabilities = (
            "host_status",
            ["host_status", "host_status"],
            [f"capability_{index}" for index in range(17)],
            ["x" * 33],
            ["测" * 11],
            [""],
            [True],
        )
        for capabilities in invalid_capabilities:
            with self.subTest(capabilities=capabilities):
                invalid = dict(message, capabilities=capabilities)
                with self.assertRaises(ProtocolError):
                    parse_device_message(json.dumps(invalid))

    def test_parses_action(self) -> None:
        message = {
            "v": 1,
            "type": "action",
            "request_id": "demo-001",
            "action_id": "option_2",
        }
        self.assertEqual(parse_device_message(json.dumps(message)), message)

    def test_parses_wallpaper_status(self) -> None:
        message = {
            "v": 1,
            "type": "wallpaper_status",
            "wallpaper_id": "0123456789abcdef",
            "state": "playing",
            "sequence": 42,
        }
        self.assertEqual(parse_device_message(json.dumps(message)), message)

    def test_rejects_invalid_wallpaper_status(self) -> None:
        for wallpaper_id, state, sequence in (
            ("../../etc/passwd", "playing", 1),
            ("0123456789ABCDEF", "playing", 1),
            ("0123456789abcdef", "unknown", 1),
            ("0123456789abcdef", "playing", -1),
            ("0123456789abcdef", "playing", True),
        ):
            with self.subTest(wallpaper_id=wallpaper_id, state=state, sequence=sequence):
                with self.assertRaises(ProtocolError):
                    parse_device_message(
                        json.dumps(
                            {
                                "v": 1,
                                "type": "wallpaper_status",
                                "wallpaper_id": wallpaper_id,
                                "state": state,
                                "sequence": sequence,
                            }
                        )
                    )

    def test_rejects_wrong_version_and_unapproved_type(self) -> None:
        with self.assertRaises(ProtocolError):
            parse_device_message('{"v":2,"type":"heartbeat","uptime_ms":1}')
        with self.assertRaises(ProtocolError):
            parse_device_message('{"v":1,"type":"shell","command":"id"}')

    def test_rejects_oversize_packet(self) -> None:
        packet = json.dumps(
            {"v": 1, "type": "heartbeat", "uptime_ms": 1, "pad": "x" * 2048}
        )
        self.assertGreater(len(packet.encode()), MAX_PACKET_BYTES)
        with self.assertRaises(ProtocolError):
            parse_device_message(packet)

    def test_demo_action_is_bounded_and_dismiss_matches(self) -> None:
        action = action_required_message("demo-001")
        self.assertEqual(len(action["actions"]), 3)
        self.assertLessEqual(len(encode_message(action).encode()), MAX_PACKET_BYTES)
        self.assertEqual(dismiss_message("demo-001")["request_id"], "demo-001")

    def test_video_control_and_binary_frame_are_bounded(self) -> None:
        record = WallpaperRecord(
            wallpaper_id="0123456789abcdef",
            name="测试动态壁纸" * 10,
            created_at="2026-09-30T00:00:00+00:00",
            duration_seconds=12.5,
            source_width=1920,
            source_height=1080,
            fps=10,
            fit="cover",
            frame_count=125,
            frame_bytes=1024,
            source_bytes=2048,
            source_media_type="video/mp4",
            jpeg_quality=9,
        )
        control = video_control_message(record)
        self.assertLessEqual(len(control["name"].encode("utf-8")), 48)
        self.assertLessEqual(len(encode_message(control).encode("utf-8")), MAX_PACKET_BYTES)

        jpeg = b"\xff\xd8" + (b"x" * 128) + b"\xff\xd9"
        packet = build_video_frame(record.wallpaper_id, 7, jpeg)
        self.assertEqual(len(packet), FRAME_HEADER_BYTES + len(jpeg))
        self.assertEqual(packet[:4], FRAME_MAGIC)
        self.assertEqual(FRAME_HEADER.size, 28)
        sequence, decoded = validate_video_frame(packet, record.wallpaper_id)
        self.assertEqual(sequence, 7)
        self.assertEqual(decoded, jpeg)

    def test_video_frame_rejects_unsafe_payloads(self) -> None:
        with self.assertRaises(ProtocolError):
            build_video_frame("0123456789abcdef", 0, b"not-a-jpeg")
        oversized = b"\xff\xd8" + (b"x" * MAX_FRAME_BYTES) + b"\xff\xd9"
        with self.assertRaises(ProtocolError):
            build_video_frame("0123456789abcdef", 0, oversized)

        valid = build_video_frame(
            "0123456789abcdef", 0, b"\xff\xd8payload\xff\xd9"
        )
        corrupted = bytearray(valid)
        corrupted[8] = 0
        with self.assertRaises(ValueError):
            validate_video_frame(bytes(corrupted), "0123456789abcdef")

    def test_host_status_message_normalizes_bounded_aggregate_fields(self) -> None:
        message = host_status_message(
            {
                "cpu_percent": 37.6,
                "memory_percent": 62.4,
                "memory_used_mb": 8192.2,
                "memory_total_mb": 16384.4,
                "uptime_seconds": 12345.7,
            }
        )
        self.assertEqual(
            message,
            {
                "v": 1,
                "type": "host_status",
                "cpu_percent": 38,
                "memory_percent": 50,
                "memory_total_mb": 16384,
                "memory_used_mb": 8192,
                "uptime_seconds": 12346,
            },
        )
        self.assertLessEqual(len(encode_message(message).encode()), MAX_PACKET_BYTES)
        self.assertEqual(validate_host_status(message), {
            "cpu_percent": 38,
            "memory_percent": 50,
            "memory_used_mb": 8192,
            "memory_total_mb": 16384,
            "uptime_seconds": 12346,
        })

        half_up = host_status_message(
            {
                "memory_percent": 12,
                "memory_used_mb": 1,
                "memory_total_mb": 8,
            }
        )
        self.assertEqual(half_up["memory_percent"], 13)
        self.assertEqual(validate_host_status(half_up)["memory_percent"], 13)

        invalid = host_status_message(
            {
                "cpu_percent": float("nan"),
                "memory_percent": 101,
                "memory_used_mb": 200,
                "memory_total_mb": 100,
                "uptime_seconds": float("inf"),
            }
        )
        self.assertEqual(
            invalid,
            {"v": 1, "type": "host_status", "memory_total_mb": 100},
        )

    def test_host_sampler_uses_cpu_deltas_and_survives_partial_failure(self) -> None:
        cpu_samples = iter(((100, 50, 850, 0), (150, 70, 930, 0)))
        sampler = HostTelemetrySampler(
            cpu_reader=lambda: next(cpu_samples),
            memory_reader=lambda: (4 * 1024**3, 8 * 1024**3),
            uptime_reader=lambda: 123.6,
        )
        first = sampler.sample()
        self.assertNotIn("cpu_percent", first)
        self.assertEqual(first["memory_percent"], 50)
        self.assertEqual(first["memory_used_mb"], 4096)
        self.assertEqual(first["memory_total_mb"], 8192)
        self.assertEqual(first["uptime_seconds"], 124)
        self.assertEqual(sampler.sample()["cpu_percent"], 47)

        def unavailable() -> Any:
            raise OSError("unavailable")

        degraded = HostTelemetrySampler(
            cpu_reader=unavailable,
            memory_reader=unavailable,
            uptime_reader=lambda: float("nan"),
        ).sample()
        self.assertEqual(degraded, {"v": 1, "type": "host_status"})

    def test_simulator_rejects_invalid_host_status(self) -> None:
        valid = {"v": 1, "type": "host_status", "cpu_percent": 0}
        self.assertEqual(validate_host_status(valid), {"cpu_percent": 0})
        for invalid in (
            {"v": 1, "type": "host_status"},
            {"v": 1, "type": "host_status", "cpu_percent": True},
            {"v": 1, "type": "host_status", "memory_percent": 101},
            {"v": True, "type": "host_status", "cpu_percent": 1},
            {
                "v": 1,
                "type": "host_status",
                "cpu_percent": None,
                "uptime_seconds": 1,
            },
            {
                "v": 1,
                "type": "host_status",
                "memory_used_mb": 2,
                "memory_total_mb": 1,
            },
            {
                "v": 1,
                "type": "host_status",
                "memory_used_mb": 0,
                "memory_total_mb": 0,
            },
            {
                "v": 1,
                "type": "host_status",
                "memory_percent": 90,
                "memory_used_mb": 1,
                "memory_total_mb": 16,
            },
            {
                "v": 1,
                "type": "host_status",
                "memory_percent": 12,
                "memory_used_mb": 1,
                "memory_total_mb": 8,
            },
            {"v": 1, "type": "host_status", "processes": ["secret"]},
        ):
            with self.subTest(message=invalid):
                with self.assertRaises(ValueError):
                    validate_host_status(invalid)

    @unittest.skipUnless(sys.platform == "darwin", "requires Mach ports")
    def test_darwin_host_statistics_do_not_leak_send_rights(self) -> None:
        library = _darwin_libsystem()
        library.mach_port_get_refs.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        library.mach_port_get_refs.restype = ctypes.c_int
        task_port = ctypes.c_uint32.in_dll(library, "mach_task_self_").value
        anchor_host_port = library.mach_host_self()
        try:
            before = ctypes.c_uint32()
            self.assertEqual(
                library.mach_port_get_refs(
                    task_port, anchor_host_port, 0, ctypes.byref(before)
                ),
                0,
            )
            for _ in range(256):
                _darwin_host_statistics(3)
                with self.assertRaises(OSError):
                    _darwin_host_statistics(-1)
            after = ctypes.c_uint32()
            self.assertEqual(
                library.mach_port_get_refs(
                    task_port, anchor_host_port, 0, ctypes.byref(after)
                ),
                0,
            )
            self.assertEqual(after.value, before.value)
        finally:
            self.assertEqual(
                library.mach_port_deallocate(task_port, anchor_host_port), 0
            )


class BridgeWallpaperMutationTests(unittest.IsolatedAsyncioTestCase):
    async def test_active_delete_closes_slow_peer_before_stopping_stream(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = write_wallpaper_record(
                root, "0123456789abcdef", "active.mp4"
            )
            library = WallpaperLibrary(root)
            library.set_active(record.wallpaper_id)
            bridge = Bridge(library)
            bridge.active_wallpaper_id = record.wallpaper_id
            connection = BlockingConnection()
            bridge.clients.add(connection)  # type: ignore[arg-type]
            bridge.ready_clients.add(connection)  # type: ignore[arg-type]
            bridge.send_locks[connection] = asyncio.Lock()  # type: ignore[index]

            with mock.patch("bridge.CLIENT_SEND_TIMEOUT_SECONDS", 0.01):
                await bridge._start_streamer(record.wallpaper_id)
                await asyncio.wait_for(connection.send_entered.wait(), timeout=1)
                result = await asyncio.wait_for(
                    bridge.delete_wallpaper(record.wallpaper_id), timeout=1
                )

            self.assertTrue(result["deleted_active"])
            self.assertFalse(connection.send_cancelled)
            self.assertEqual(connection.close_calls, [(1011, "send timeout")])
            self.assertNotIn(connection, bridge.clients)
            self.assertNotIn(connection, bridge.ready_clients)
            self.assertIsNone(bridge.stream_task)
            self.assertIsNone(bridge.active_wallpaper_id)

    async def test_delete_inactive_preserves_stream_and_active_delete_falls_back(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            active = write_wallpaper_record(
                root, "0123456789abcdef", "active.mp4"
            )
            inactive = write_wallpaper_record(
                root, "fedcba9876543210", "wrong.mp4"
            )
            library = WallpaperLibrary(root)
            library.set_active(active.wallpaper_id)
            bridge = Bridge(library)
            bridge.active_wallpaper_id = active.wallpaper_id
            await bridge._start_streamer(active.wallpaper_id)
            active_task = bridge.stream_task
            marker = object()
            bridge.device_status[marker] = {"state": "playing"}  # type: ignore[index]

            inactive_result = await bridge.delete_wallpaper(
                inactive.wallpaper_id
            )
            self.assertFalse(inactive_result["deleted_active"])
            self.assertEqual(
                inactive_result["wallpaper_state"], "unchanged"
            )
            self.assertEqual(bridge.active_wallpaper_id, active.wallpaper_id)
            self.assertIs(bridge.stream_task, active_task)
            self.assertIn(marker, bridge.device_status)

            active_result = await bridge.delete_wallpaper(active.wallpaper_id)
            self.assertTrue(active_result["deleted_active"])
            self.assertEqual(active_result["wallpaper_state"], "builtin")
            self.assertIsNone(active_result["active_id"])
            self.assertFalse(active_result["cleanup_pending"])
            self.assertIsNone(bridge.active_wallpaper_id)
            self.assertIsNone(bridge.stream_task)
            self.assertEqual(bridge.device_status, {})
            self.assertEqual((await bridge.status_snapshot())["wallpaper_state"], "builtin")

    async def test_delete_failure_restores_previous_active_stream(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = write_wallpaper_record(
                root, "0123456789abcdef", "active.mp4"
            )
            library = WallpaperLibrary(root)
            library.set_active(record.wallpaper_id)
            bridge = Bridge(library)
            bridge.active_wallpaper_id = record.wallpaper_id
            await bridge._start_streamer(record.wallpaper_id)

            with mock.patch.object(
                library,
                "delete",
                side_effect=WallpaperStorageError("delete failed"),
            ):
                with self.assertRaises(WallpaperStorageError):
                    await bridge.delete_wallpaper(record.wallpaper_id)

            self.assertEqual(bridge.active_wallpaper_id, record.wallpaper_id)
            self.assertIsNotNone(bridge.stream_task)
            self.assertEqual(library.active_id(), record.wallpaper_id)
            await bridge._stop_streamer()

    async def test_activate_and_delete_are_serialized_without_stale_active_window(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = write_wallpaper_record(
                root, "0123456789abcdef", "first.mp4"
            )
            second = write_wallpaper_record(
                root, "fedcba9876543210", "second.mp4"
            )
            library = WallpaperLibrary(root)
            library.set_active(first.wallpaper_id)
            bridge = Bridge(library)
            bridge.active_wallpaper_id = first.wallpaper_id
            await bridge._start_streamer(first.wallpaper_id)

            entered_stop = asyncio.Event()
            release_stop = asyncio.Event()
            original_stop = bridge._stop_streamer

            async def paused_stop() -> None:
                entered_stop.set()
                await release_stop.wait()
                await original_stop()

            bridge._stop_streamer = paused_stop  # type: ignore[method-assign]
            activate_task = asyncio.create_task(
                bridge.activate_wallpaper(second.wallpaper_id)
            )
            await entered_stop.wait()
            self.assertIsNone(bridge.active_wallpaper_id)

            delete_task = asyncio.create_task(
                bridge.delete_wallpaper(second.wallpaper_id)
            )
            await asyncio.sleep(0)
            self.assertFalse(delete_task.done())
            release_stop.set()
            await activate_task
            await delete_task

            self.assertIsNone(bridge.active_wallpaper_id)
            self.assertIsNone(bridge.stream_task)
            self.assertEqual(library.active_id(), None)
            self.assertEqual(library.get(first.wallpaper_id), first)
            with self.assertRaises(WallpaperNotFoundError):
                library.get(second.wallpaper_id)


class BridgeTelemetryTests(unittest.IsolatedAsyncioTestCase):
    async def test_host_status_is_capability_gated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            now = [100.0]
            bridge = Bridge(
                WallpaperLibrary(Path(directory)),
                monotonic_clock=lambda: now[0],
            )
            bridge.latest_host_status = host_status_message(
                {"cpu_percent": 25, "memory_percent": 50}
            )
            bridge.latest_host_status_sampled_at = now[0]
            capable = FakeConnection()
            legacy = FakeConnection()
            for connection in (capable, legacy):
                bridge.clients.add(connection)  # type: ignore[arg-type]
                bridge.send_locks[connection] = asyncio.Lock()  # type: ignore[index]

            base_hello = {
                "v": 1,
                "type": "hello",
                "device": "esp32-companion-simulator",
                "width": 480,
                "height": 320,
                "firmware": "simulator",
            }
            await bridge.handle_device_message(
                capable,  # type: ignore[arg-type]
                dict(base_hello, capabilities=[HOST_STATUS_CAPABILITY]),
            )
            await bridge.handle_device_message(legacy, base_hello)  # type: ignore[arg-type]

            self.assertEqual(len(capable.sent), 1)
            self.assertEqual(json.loads(capable.sent[0])["type"], "host_status")
            self.assertEqual(legacy.sent, [])

            delivered = await bridge.broadcast(
                host_status_message({"cpu_percent": 30}),
                "host_status",
                capability=HOST_STATUS_CAPABILITY,
            )
            self.assertEqual(delivered, 1)
            self.assertEqual(len(capable.sent), 2)
            self.assertEqual(legacy.sent, [])

    async def test_stale_and_failed_host_status_samples_clear_cache(self) -> None:
        valid = host_status_message({"cpu_percent": 25})
        empty = {"v": 1, "type": "host_status"}
        sampler = FakeHostSampler(
            [valid, empty, valid, OSError("sampling failed")]
        )
        now = [100.0]
        with tempfile.TemporaryDirectory() as directory:
            bridge = Bridge(
                WallpaperLibrary(Path(directory)),
                host_sampler=sampler,  # type: ignore[arg-type]
                monotonic_clock=lambda: now[0],
            )
            self.assertEqual(await bridge._sample_host_status(), valid)
            self.assertEqual(bridge.latest_host_status_sampled_at, 100.0)

            now[0] = 115.0
            self.assertEqual(bridge._fresh_cached_host_status(), valid)
            now[0] = 115.001
            stale_client = FakeConnection()
            bridge.clients.add(stale_client)  # type: ignore[arg-type]
            bridge.send_locks[stale_client] = asyncio.Lock()  # type: ignore[index]
            await bridge.handle_device_message(
                stale_client,  # type: ignore[arg-type]
                {
                    "v": 1,
                    "type": "hello",
                    "device": "esp32-companion-simulator",
                    "width": 480,
                    "height": 320,
                    "firmware": "simulator",
                    "capabilities": [HOST_STATUS_CAPABILITY],
                },
            )
            self.assertEqual(stale_client.sent, [])
            self.assertIsNone(bridge.latest_host_status)
            self.assertIsNone(bridge.latest_host_status_sampled_at)

            self.assertIsNone(await bridge._sample_host_status())
            self.assertIsNone(bridge.latest_host_status)
            self.assertIsNone(bridge.latest_host_status_sampled_at)

            now[0] = 200.0
            self.assertEqual(await bridge._sample_host_status(), valid)
            self.assertEqual(bridge.latest_host_status_sampled_at, 200.0)
            self.assertIsNone(await bridge._sample_host_status())
            self.assertIsNone(bridge.latest_host_status)
            self.assertIsNone(bridge.latest_host_status_sampled_at)


if __name__ == "__main__":
    unittest.main()
