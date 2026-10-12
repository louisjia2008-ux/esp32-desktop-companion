from __future__ import annotations

from dataclasses import asdict
from concurrent.futures import Future
import http.client
from io import BytesIO
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from wallpaper_http import EditorRequestHandler, WallpaperEditorService
from wallpaper_library import (
    MAX_UPLOAD_BYTES,
    WallpaperError,
    WallpaperLibrary,
    WallpaperNotFoundError,
    WallpaperRecord,
    WallpaperStorageError,
    copy_bounded_upload,
    normalize_name,
    validate_fit,
    validate_fps,
)


def write_record(root: Path, wallpaper_id: str, name: str) -> WallpaperRecord:
    record = WallpaperRecord(
        wallpaper_id=wallpaper_id,
        name=name,
        created_at=f"2026-09-30T00:00:{wallpaper_id[-2:]}+00:00",
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


def resolved_future(value: object = None, error: Exception | None = None) -> Future:
    future: Future = Future()
    if error is not None:
        future.set_exception(error)
    else:
        future.set_result(value)
    return future


class WallpaperLibraryTests(unittest.TestCase):
    def test_validates_user_options(self) -> None:
        self.assertEqual(normalize_name("  demo   video.mp4  "), "demo video.mp4")
        self.assertEqual(validate_fit("cover"), "cover")
        self.assertEqual(validate_fps("10"), 10)
        for invalid in ("stretch", "", "../cover"):
            with self.assertRaises(WallpaperError):
                validate_fit(invalid)
        for invalid in (0, 7, 60, True, "ten"):
            with self.assertRaises(WallpaperError):
                validate_fps(invalid)

    def test_bounded_upload_requires_exact_declared_body(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "upload"
            copy_bounded_upload(BytesIO(b"video"), target, 5)
            self.assertEqual(target.read_bytes(), b"video")
            with self.assertRaises(WallpaperError):
                copy_bounded_upload(BytesIO(b"tiny"), Path(temporary) / "short", 8)
            with self.assertRaises(WallpaperError):
                copy_bounded_upload(
                    BytesIO(b"x"), Path(temporary) / "large", MAX_UPLOAD_BYTES + 1
                )

    def test_ten_minute_probe_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            library = WallpaperLibrary(Path(temporary))
            for duration in (600, 600.001, 0, "nan"):
                probe = {"streams": [{"codec_type": "video", "width": 640,
                                      "height": 360, "duration": str(duration)}]}
                with self.subTest(duration=duration), mock.patch(
                    "wallpaper_library.subprocess.run",
                    return_value=mock.Mock(stdout=json.dumps(probe)),
                ):
                    if duration == 600:
                        self.assertEqual(library._probe(Path("source.mp4"))["duration"], 600)
                    else:
                        with self.assertRaisesRegex(WallpaperError, "10 分钟"):
                            library._probe(Path("source.mp4"))

    def test_library_uses_only_strict_random_ids(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            library = WallpaperLibrary(Path(temporary))
            record = WallpaperRecord(
                wallpaper_id="0123456789abcdef",
                name="demo.mp4",
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
            directory = Path(temporary) / record.wallpaper_id
            (directory / "frames").mkdir(parents=True)
            (directory / "source.bin").write_bytes(b"source")
            (directory / "thumbnail.jpg").write_bytes(b"thumb")
            (directory / "frames" / "000001.jpg").write_bytes(b"\xff\xd8xx\xff\xd9")
            (directory / "metadata.json").write_text(
                json.dumps(asdict(record)), encoding="utf-8"
            )

            self.assertEqual(library.get(record.wallpaper_id), record)
            self.assertEqual(library.set_active(record.wallpaper_id), record)
            self.assertEqual(library.active_id(), record.wallpaper_id)
            self.assertEqual(library.frame_path(record.wallpaper_id, 0).name, "000001.jpg")
            for invalid in ("../0123456789abcdef", "0123456789ABCDEf", "short"):
                with self.assertRaises(WallpaperError):
                    library.get(invalid)

    def test_http_range_parser_is_strict(self) -> None:
        parse = EditorRequestHandler._parse_range
        self.assertEqual(parse("bytes=0-9", 100), (0, 9))
        self.assertEqual(parse("bytes=90-", 100), (90, 99))
        self.assertEqual(parse("bytes=-10", 100), (90, 99))
        self.assertIsNone(parse("bytes=100-101", 100))
        self.assertIsNone(parse("bytes=0-1,3-4", 100))
        self.assertIsNone(parse("items=0-1", 100))

    def test_delete_inactive_and_active_records_atomically(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            active = write_record(root, "0123456789abcdef", "active.mp4")
            inactive = write_record(root, "fedcba9876543210", "wrong.mp4")
            library = WallpaperLibrary(root)
            library.set_active(active.wallpaper_id)

            deleted, was_active, cleanup_complete = library.delete(
                inactive.wallpaper_id
            )
            self.assertEqual(deleted, inactive)
            self.assertFalse(was_active)
            self.assertTrue(cleanup_complete)
            self.assertEqual(library.active_id(), active.wallpaper_id)
            self.assertFalse((root / inactive.wallpaper_id).exists())
            with self.assertRaises(WallpaperNotFoundError):
                library.get(inactive.wallpaper_id)

            deleted, was_active, cleanup_complete = library.delete(
                active.wallpaper_id
            )
            self.assertEqual(deleted, active)
            self.assertTrue(was_active)
            self.assertTrue(cleanup_complete)
            self.assertIsNone(library.active_id())
            self.assertFalse(library.active_file.exists())
            self.assertEqual(library.list_records(), [])

    def test_delete_rejects_unsafe_ids_and_symlink_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            root = base / "library"
            external = base / "external"
            external.mkdir()
            sentinel = external / "keep.txt"
            sentinel.write_text("keep", encoding="utf-8")
            library = WallpaperLibrary(root)
            symlink_id = "0123456789abcdef"
            (root / symlink_id).symlink_to(external, target_is_directory=True)

            for invalid in ("../external", "0123456789ABCDEf", "short"):
                with self.subTest(wallpaper_id=invalid):
                    with self.assertRaises(WallpaperError):
                        library.delete(invalid)
            with self.assertRaises(WallpaperNotFoundError):
                library.delete(symlink_id)
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")

    def test_delete_commit_failure_preserves_visible_record(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = write_record(root, "0123456789abcdef", "keep.mp4")
            library = WallpaperLibrary(root)
            library.set_active(record.wallpaper_id)
            with mock.patch(
                "wallpaper_library.os.replace", side_effect=OSError("denied")
            ):
                with self.assertRaises(WallpaperStorageError):
                    library.delete(record.wallpaper_id)
            self.assertEqual(library.get(record.wallpaper_id), record)
            self.assertEqual(library.active_id(), record.wallpaper_id)

    def test_delete_active_pointer_read_failure_preserves_visible_record(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = write_record(root, "0123456789abcdef", "keep.mp4")
            library = WallpaperLibrary(root)
            library.set_active(record.wallpaper_id)
            real_read_text = Path.read_text

            def fail_active_read(
                path: Path, *args: object, **kwargs: object
            ) -> str:
                if path == library.active_file:
                    raise OSError("busy")
                return real_read_text(path, *args, **kwargs)

            with mock.patch.object(Path, "read_text", fail_active_read):
                with self.assertRaises(WallpaperStorageError):
                    library.delete(record.wallpaper_id)

            self.assertEqual(library.get(record.wallpaper_id), record)
            self.assertEqual(library.active_id(), record.wallpaper_id)
            self.assertEqual(list(root.glob(".deleted-*.tmp")), [])

    def test_delete_cleanup_failure_leaves_hidden_retryable_tombstone(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = write_record(root, "0123456789abcdef", "remove.mp4")
            library = WallpaperLibrary(root)
            with mock.patch(
                "wallpaper_library.shutil.rmtree", side_effect=OSError("busy")
            ):
                _, _, cleanup_complete = library.delete(record.wallpaper_id)
            self.assertFalse(cleanup_complete)
            self.assertFalse((root / record.wallpaper_id).exists())
            tombstones = list(root.glob(".deleted-*.tmp"))
            self.assertEqual(len(tombstones), 1)
            self.assertEqual(library.list_records(), [])

            restarted = WallpaperLibrary(root)
            self.assertEqual(restarted.list_records(), [])
            self.assertFalse(tombstones[0].exists())

    def test_open_media_handle_remains_readable_after_logical_delete(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = write_record(root, "0123456789abcdef", "open.mp4")
            library = WallpaperLibrary(root)
            opened_record, handle = library.open_source(record.wallpaper_id)
            self.assertEqual(opened_record, record)
            library.delete(record.wallpaper_id)
            with handle:
                self.assertEqual(handle.read(), b"source")
            self.assertEqual(library.list_records(), [])

    def test_stale_active_pointer_is_safe_and_retried_on_restart(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = write_record(root, "0123456789abcdef", "active.mp4")
            library = WallpaperLibrary(root)
            library.set_active(record.wallpaper_id)
            real_unlink = Path.unlink

            def fail_active_unlink(path: Path, *args: object, **kwargs: object) -> None:
                if path == library.active_file:
                    raise OSError("busy")
                real_unlink(path, *args, **kwargs)

            with mock.patch.object(Path, "unlink", fail_active_unlink):
                _, was_active, cleanup_complete = library.delete(
                    record.wallpaper_id
                )
            self.assertTrue(was_active)
            self.assertFalse(cleanup_complete)
            self.assertTrue(library.active_file.exists())
            self.assertIsNone(library.active_id())

            restarted = WallpaperLibrary(root)
            self.assertIsNone(restarted.active_id())
            self.assertFalse(restarted.active_file.exists())

    def test_http_delete_route_is_strict_and_reconciles_result(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "library"
            record = write_record(root, "0123456789abcdef", "remove.mp4")
            storage_failure = write_record(
                root, "1111111111111111", "keep-on-error.mp4"
            )
            timeout_record = write_record(
                root, "2222222222222222", "unknown.mp4"
            )
            library = WallpaperLibrary(root)
            calls: list[str] = []

            def delete_callback(wallpaper_id: str) -> Future:
                calls.append(wallpaper_id)
                if wallpaper_id == storage_failure.wallpaper_id:
                    return resolved_future(
                        error=WallpaperStorageError("无法删除动态壁纸")
                    )
                if wallpaper_id == timeout_record.wallpaper_id:
                    return Future()
                try:
                    deleted, was_active, cleanup_complete = library.delete(
                        wallpaper_id
                    )
                except Exception as error:
                    return resolved_future(error=error)
                return resolved_future(
                    {
                        "deleted_id": deleted.wallpaper_id,
                        "deleted_name": deleted.name,
                        "deleted_active": was_active,
                        "active_id": library.active_id(),
                        "wallpaper_state": "builtin" if was_active else "unchanged",
                        "cleanup_pending": not cleanup_complete,
                    }
                )

            try:
                service = WallpaperEditorService(
                    library,
                    Path(__file__).resolve().parents[1],
                    0,
                    lambda _wallpaper_id: resolved_future({}),
                    lambda: resolved_future({"bridge": "ready"}),
                    delete_callback,
                )
            except PermissionError:
                self.skipTest("loopback bind is unavailable in this sandbox")
            service.start()
            try:
                connection = http.client.HTTPConnection(
                    "127.0.0.1", service.port, timeout=3
                )
                connection.request(
                    "DELETE", f"/api/wallpapers/{record.wallpaper_id}"
                )
                response = connection.getresponse()
                payload = json.loads(response.read())
                self.assertEqual(response.status, 200)
                self.assertEqual(response.getheader("Cache-Control"), "no-store")
                self.assertEqual(response.getheader("X-Frame-Options"), "DENY")
                self.assertEqual(payload["deleted_id"], record.wallpaper_id)
                self.assertFalse(payload["cleanup_pending"])
                connection.close()

                connection = http.client.HTTPConnection(
                    "127.0.0.1", service.port, timeout=3
                )
                connection.request(
                    "DELETE", f"/api/wallpapers/{record.wallpaper_id}"
                )
                response = connection.getresponse()
                payload = json.loads(response.read())
                self.assertEqual(response.status, 404)
                self.assertEqual(payload["code"], "not_found")
                connection.close()

                connection = http.client.HTTPConnection(
                    "127.0.0.1", service.port, timeout=3
                )
                connection.request("DELETE", "/api/wallpapers/../outside")
                response = connection.getresponse()
                payload = json.loads(response.read())
                self.assertEqual(response.status, 400)
                self.assertEqual(payload["code"], "invalid_wallpaper_id")
                connection.close()

                connection = http.client.HTTPConnection(
                    "127.0.0.1", service.port, timeout=3
                )
                connection.request(
                    "DELETE",
                    "/api/wallpapers/fedcba9876543210",
                    body=b"x",
                )
                response = connection.getresponse()
                payload = json.loads(response.read())
                self.assertEqual(response.status, 400)
                self.assertEqual(payload["code"], "invalid_request_body")
                connection.close()

                connection = http.client.HTTPConnection(
                    "127.0.0.1", service.port, timeout=3
                )
                connection.request(
                    "DELETE",
                    f"/api/wallpapers/{storage_failure.wallpaper_id}",
                )
                response = connection.getresponse()
                payload = json.loads(response.read())
                self.assertEqual(response.status, 500)
                self.assertEqual(payload["code"], "delete_failed")
                self.assertEqual(
                    library.get(storage_failure.wallpaper_id), storage_failure
                )
                connection.close()

                with mock.patch(
                    "wallpaper_http.CONTROL_TIMEOUT_SECONDS", 0.01
                ):
                    connection = http.client.HTTPConnection(
                        "127.0.0.1", service.port, timeout=3
                    )
                    connection.request(
                        "DELETE",
                        f"/api/wallpapers/{timeout_record.wallpaper_id}",
                    )
                    response = connection.getresponse()
                    payload = json.loads(response.read())
                    self.assertEqual(response.status, 503)
                    self.assertEqual(payload["code"], "outcome_unknown")
                    connection.close()
            finally:
                service.stop()

            self.assertEqual(
                calls,
                [
                    record.wallpaper_id,
                    record.wallpaper_id,
                    storage_failure.wallpaper_id,
                    timeout_record.wallpaper_id,
                ],
            )


if __name__ == "__main__":
    unittest.main()
