#!/usr/bin/env python3
"""Validated local video-wallpaper storage and ESP32 frame transcoding."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import json
import mimetypes
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import threading
from typing import Any, BinaryIO

from wallpaper_cache import MAX_PACKAGE_DURATION_SECONDS


WALLPAPER_ID_PATTERN = re.compile(r"^[0-9a-f]{16}$")
DELETE_TOMBSTONE_PATTERN = re.compile(
    r"^\.deleted-[0-9a-f]{16}-[0-9a-f]{16}\.tmp$"
)
ALLOWED_FIT_MODES = frozenset({"cover", "contain"})
ALLOWED_FRAME_RATES = frozenset({6, 8, 10, 12})
DEFAULT_FRAME_RATE = 10
DEFAULT_FIT_MODE = "cover"
MAX_UPLOAD_BYTES = 800 * 1024 * 1024
MAX_DURATION_SECONDS = float(MAX_PACKAGE_DURATION_SECONDS)
MAX_SOURCE_DIMENSION = 8192
MAX_SOURCE_PIXELS = 33_554_432
MAX_FRAME_BYTES = 14 * 1024
MAX_TRANSCODE_BYTES = 128 * 1024 * 1024
SAFE_DEMUXERS = "mov,matroska,webm,avi"
NATIVE_WIDTH = 320
NATIVE_HEIGHT = 480
LOGICAL_WIDTH = 480
LOGICAL_HEIGHT = 320


class WallpaperError(ValueError):
    """A safe, user-displayable wallpaper validation error."""


class WallpaperNotFoundError(WallpaperError):
    """The requested library record is absent or no longer usable."""


class WallpaperStorageError(WallpaperError):
    """A library mutation could not reach its logical commit point."""


@dataclass(frozen=True)
class WallpaperRecord:
    wallpaper_id: str
    name: str
    created_at: str
    duration_seconds: float
    source_width: int
    source_height: int
    fps: int
    fit: str
    frame_count: int
    frame_bytes: int
    source_bytes: int
    source_media_type: str
    jpeg_quality: int

    @property
    def width(self) -> int:
        return NATIVE_WIDTH

    @property
    def height(self) -> int:
        return NATIVE_HEIGHT

    def public_dict(self, *, active: bool = False) -> dict[str, Any]:
        result = asdict(self)
        result.update(
            {
                "id": result.pop("wallpaper_id"),
                "width": self.width,
                "height": self.height,
                "active": active,
                "thumbnail_url": f"/thumb/{self.wallpaper_id}",
                "media_url": f"/media/{self.wallpaper_id}",
            }
        )
        return result


def normalize_name(name: str) -> str:
    """Keep a short display label; never use it as a filesystem path."""
    if not isinstance(name, str):
        raise WallpaperError("文件名无效")
    cleaned = " ".join(name.replace("\x00", "").split())
    if not cleaned:
        cleaned = "未命名动态壁纸"
    # Count Unicode code points rather than encoded bytes for predictable UI.
    return cleaned[:120]


def validate_fit(fit: str) -> str:
    if fit not in ALLOWED_FIT_MODES:
        raise WallpaperError("适配方式只能是 cover 或 contain")
    return fit


def validate_fps(value: int | str) -> int:
    try:
        fps = int(value)
    except (TypeError, ValueError) as error:
        raise WallpaperError("帧率无效") from error
    if isinstance(value, bool) or fps not in ALLOWED_FRAME_RATES:
        raise WallpaperError("帧率只能选择 6、8、10 或 12 FPS")
    return fps


def _strict_wallpaper_id(value: str) -> str:
    if not isinstance(value, str) or WALLPAPER_ID_PATTERN.fullmatch(value) is None:
        raise WallpaperError("动态壁纸 ID 无效")
    return value


def _safe_media_type(name: str) -> str:
    guessed, _ = mimetypes.guess_type(name)
    if guessed and guessed.startswith("video/"):
        return guessed
    return "application/octet-stream"


class WallpaperLibrary:
    """Thread-safe, bounded local library stored beneath one fixed root."""

    def __init__(
        self,
        root: Path,
        *,
        ffmpeg: str | None = None,
        ffprobe: str | None = None,
    ) -> None:
        self.root = root.resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.incoming_root = self.root / ".incoming"
        self.incoming_root.mkdir(exist_ok=True)
        self.ffmpeg = ffmpeg or shutil.which("ffmpeg") or "ffmpeg"
        self.ffprobe = ffprobe or shutil.which("ffprobe") or "ffprobe"
        self._lock = threading.RLock()
        self._purge_delete_tombstones()
        self._purge_stale_active_pointer()

    @property
    def active_file(self) -> Path:
        return self.root / "active.json"

    def new_upload_path(self) -> Path:
        return self.incoming_root / f"{secrets.token_hex(16)}.upload"

    def discard_upload(self, path: Path) -> None:
        try:
            resolved = path.resolve()
            resolved.relative_to(self.incoming_root)
        except (OSError, ValueError):
            return
        try:
            resolved.unlink(missing_ok=True)
        except OSError:
            pass

    def list_records(self) -> list[WallpaperRecord]:
        with self._lock:
            return self._list_records_locked()

    def snapshot(self) -> tuple[list[WallpaperRecord], str | None]:
        """Return records and their active id from one consistent lock view."""
        with self._lock:
            records = self._list_records_locked()
            active_id = self.active_id()
            return records, active_id

    def get(self, wallpaper_id: str) -> WallpaperRecord:
        wallpaper_id = _strict_wallpaper_id(wallpaper_id)
        with self._lock:
            return self._read_record(wallpaper_id)

    def active_id(self) -> str | None:
        with self._lock:
            try:
                payload = json.loads(self.active_file.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                return None
            value = payload.get("id") if isinstance(payload, dict) else None
            if not isinstance(value, str) or WALLPAPER_ID_PATTERN.fullmatch(value) is None:
                return None
            try:
                self._read_record(value)
            except WallpaperError:
                return None
            return value

    def set_active(self, wallpaper_id: str) -> WallpaperRecord:
        wallpaper_id = _strict_wallpaper_id(wallpaper_id)
        with self._lock:
            record = self._read_record(wallpaper_id)
            temporary = self.root / f".active-{secrets.token_hex(8)}.tmp"
            temporary.write_text(
                json.dumps({"id": wallpaper_id}, separators=(",", ":")),
                encoding="utf-8",
            )
            os.replace(temporary, self.active_file)
            return record

    def delete(self, wallpaper_id: str) -> tuple[WallpaperRecord, bool, bool]:
        """Atomically remove one record from the visible library.

        The directory is renamed to a strictly named tombstone before it is
        recursively removed. That keeps readers from observing a half-deleted
        record. A rare cleanup failure leaves only the hidden tombstone, which
        is retried when the library next starts; callers can report that state
        truthfully without making the deleted wallpaper selectable again.
        """

        wallpaper_id = _strict_wallpaper_id(wallpaper_id)
        with self._lock:
            record = self._read_record(wallpaper_id)
            try:
                active_payload = json.loads(
                    self.active_file.read_text(encoding="utf-8")
                )
            except FileNotFoundError:
                active_payload = None
            except json.JSONDecodeError:
                active_payload = None
            except OSError as error:
                # Failing closed here matters: if we guessed "inactive" and
                # committed the directory rename, active.json could be left
                # pointing at a record that no longer exists while the API
                # incorrectly reported a clean inactive deletion.
                raise WallpaperStorageError(
                    "无法读取当前动态壁纸状态"
                ) from error
            active_value = (
                active_payload.get("id")
                if isinstance(active_payload, dict)
                else None
            )
            was_active = active_value == wallpaper_id
            directory = self.root / wallpaper_id
            tombstone = (
                self.root
                / f".deleted-{wallpaper_id}-{secrets.token_hex(8)}.tmp"
            )
            try:
                os.replace(directory, tombstone)
            except OSError as error:
                raise WallpaperStorageError("无法删除动态壁纸") from error

            pointer_cleanup_complete = True
            if was_active:
                try:
                    self.active_file.unlink(missing_ok=True)
                except OSError:
                    # The directory rename above is the logical commit point.
                    # active_id() already returns None because the record is no
                    # longer visible, so a stale pointer is safe to retry later.
                    pointer_cleanup_complete = False

        cleanup_complete = self._remove_delete_tombstone(tombstone)
        return (
            record,
            was_active,
            pointer_cleanup_complete and cleanup_complete,
        )

    def sd_package(self, wallpaper_id: str):
        from wallpaper_cache import CachePackageError, build_package
        with self._lock:
            record = self.get(wallpaper_id)
            try:
                return build_package(
                    record, lambda index: self.read_frame(wallpaper_id, index),
                    self.root / record.wallpaper_id / "wallpaper.dwc",
                )
            except CachePackageError as error:
                raise WallpaperError(str(error)) from error

    def frame_path(self, wallpaper_id: str, index: int) -> Path:
        record = self.get(wallpaper_id)
        if not isinstance(index, int) or index < 0 or index >= record.frame_count:
            raise WallpaperError("动态壁纸帧序号无效")
        return self.root / record.wallpaper_id / "frames" / f"{index + 1:06d}.jpg"

    def read_frame(self, wallpaper_id: str, index: int) -> bytes:
        """Read one small transcoded frame while holding the deletion lock."""
        with self._lock:
            path = self.frame_path(wallpaper_id, index)
            try:
                return path.read_bytes()
            except OSError as error:
                raise WallpaperError("动态壁纸帧不可读") from error

    def thumbnail_path(self, wallpaper_id: str) -> Path:
        record = self.get(wallpaper_id)
        return self.root / record.wallpaper_id / "thumbnail.jpg"

    def source_path(self, wallpaper_id: str) -> Path:
        record = self.get(wallpaper_id)
        return self.root / record.wallpaper_id / "source.bin"

    def open_thumbnail(self, wallpaper_id: str) -> BinaryIO:
        with self._lock:
            record = self._read_record(_strict_wallpaper_id(wallpaper_id))
            try:
                return (self.root / record.wallpaper_id / "thumbnail.jpg").open("rb")
            except OSError as error:
                raise WallpaperError("动态壁纸预览图不可读") from error

    def open_source(self, wallpaper_id: str) -> tuple[WallpaperRecord, BinaryIO]:
        with self._lock:
            record = self._read_record(_strict_wallpaper_id(wallpaper_id))
            try:
                handle = (self.root / record.wallpaper_id / "source.bin").open("rb")
            except OSError as error:
                raise WallpaperError("动态壁纸源文件不可读") from error
            return record, handle

    def import_upload(
        self,
        upload_path: Path,
        *,
        display_name: str,
        fit: str = DEFAULT_FIT_MODE,
        fps: int = DEFAULT_FRAME_RATE,
    ) -> WallpaperRecord:
        """Validate and atomically transcode a previously bounded upload."""
        display_name = normalize_name(display_name)
        fit = validate_fit(fit)
        fps = validate_fps(fps)
        try:
            source_bytes = upload_path.stat().st_size
        except OSError as error:
            raise WallpaperError("上传文件不可读") from error
        if source_bytes <= 0 or source_bytes > MAX_UPLOAD_BYTES:
            raise WallpaperError("文件必须大于 0 且不超过 800 MB")

        probe = self._probe(upload_path)
        duration = probe["duration"]
        width = probe["width"]
        height = probe["height"]

        wallpaper_id = self._allocate_id()
        staging = self.root / f".{wallpaper_id}-{secrets.token_hex(8)}.tmp"
        final = self.root / wallpaper_id
        frames = staging / "frames"
        try:
            frames.mkdir(parents=True)
            shutil.copyfile(upload_path, staging / "source.bin")
            quality, frame_files, total_bytes = self._transcode_frames(
                staging / "source.bin", frames, fit=fit, fps=fps
            )
            self._make_thumbnail(staging / "source.bin", staging / "thumbnail.jpg", fit)
            created_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
            record = WallpaperRecord(
                wallpaper_id=wallpaper_id,
                name=display_name,
                created_at=created_at,
                duration_seconds=round(duration, 3),
                source_width=width,
                source_height=height,
                fps=fps,
                fit=fit,
                frame_count=len(frame_files),
                frame_bytes=total_bytes,
                source_bytes=source_bytes,
                source_media_type=_safe_media_type(display_name),
                jpeg_quality=quality,
            )
            (staging / "metadata.json").write_text(
                json.dumps(asdict(record), ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
            os.replace(staging, final)
            return record
        except WallpaperError:
            shutil.rmtree(staging, ignore_errors=True)
            raise
        except (OSError, subprocess.SubprocessError) as error:
            shutil.rmtree(staging, ignore_errors=True)
            raise WallpaperError("动态壁纸处理失败") from error
        finally:
            self.discard_upload(upload_path)

    def _allocate_id(self) -> str:
        for _ in range(32):
            candidate = secrets.token_hex(8)
            if not (self.root / candidate).exists():
                return candidate
        raise WallpaperError("无法分配动态壁纸 ID")

    def _read_record(self, wallpaper_id: str) -> WallpaperRecord:
        directory = self.root / _strict_wallpaper_id(wallpaper_id)
        if directory.is_symlink() or not directory.is_dir():
            raise WallpaperNotFoundError("动态壁纸不存在或记录损坏")
        try:
            payload = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
            record = WallpaperRecord(**payload)
        except (OSError, json.JSONDecodeError, TypeError, ValueError) as error:
            raise WallpaperNotFoundError("动态壁纸不存在或记录损坏") from error
        if (
            record.wallpaper_id != wallpaper_id
            or record.fit not in ALLOWED_FIT_MODES
            or record.fps not in ALLOWED_FRAME_RATES
            or record.frame_count <= 0
            or record.frame_bytes <= 0
            or record.source_bytes <= 0
        ):
            raise WallpaperNotFoundError("动态壁纸记录无效")
        if not (directory / "source.bin").is_file() or not (directory / "thumbnail.jpg").is_file():
            raise WallpaperNotFoundError("动态壁纸文件缺失")
        return record

    def _list_records_locked(self) -> list[WallpaperRecord]:
        records: list[WallpaperRecord] = []
        for entry in self.root.iterdir():
            if (
                not entry.is_dir()
                or entry.is_symlink()
                or WALLPAPER_ID_PATTERN.fullmatch(entry.name) is None
            ):
                continue
            try:
                records.append(self._read_record(entry.name))
            except WallpaperError:
                continue
        records.sort(key=lambda record: record.created_at, reverse=True)
        return records

    def _purge_delete_tombstones(self) -> None:
        for entry in self.root.iterdir():
            if DELETE_TOMBSTONE_PATTERN.fullmatch(entry.name) is not None:
                self._remove_delete_tombstone(entry)

    def _purge_stale_active_pointer(self) -> None:
        if not self.active_file.exists():
            return
        stale = False
        try:
            payload = json.loads(self.active_file.read_text(encoding="utf-8"))
            value = payload.get("id") if isinstance(payload, dict) else None
            if (
                not isinstance(value, str)
                or WALLPAPER_ID_PATTERN.fullmatch(value) is None
            ):
                stale = True
            else:
                self._read_record(value)
        except (OSError, json.JSONDecodeError, WallpaperError):
            stale = True
        if stale:
            try:
                self.active_file.unlink(missing_ok=True)
            except OSError:
                pass

    @staticmethod
    def _remove_delete_tombstone(path: Path) -> bool:
        try:
            if path.is_symlink():
                path.unlink(missing_ok=True)
            else:
                shutil.rmtree(path)
            return True
        except FileNotFoundError:
            # Cleanup is idempotent. Another startup or cleanup worker may
            # already have removed the hidden tombstone.
            return True
        except OSError:
            return False

    def _probe(self, source: Path) -> dict[str, int | float]:
        command = [
            self.ffprobe,
            "-v",
            "error",
            "-protocol_whitelist",
            "file,pipe",
            "-format_whitelist",
            SAFE_DEMUXERS,
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_type,width,height,duration:format=duration",
            "-of",
            "json",
            str(source),
        ]
        try:
            result = subprocess.run(
                command,
                check=True,
                capture_output=True,
                text=True,
                timeout=45,
            )
            payload = json.loads(result.stdout)
            streams = payload.get("streams")
            stream = streams[0] if isinstance(streams, list) and streams else None
            if not isinstance(stream, dict) or stream.get("codec_type") != "video":
                raise WallpaperError("文件中没有可用的视频轨道")
            width = int(stream.get("width"))
            height = int(stream.get("height"))
            raw_duration = stream.get("duration") or payload.get("format", {}).get("duration")
            duration = float(raw_duration)
        except WallpaperError:
            raise
        except (FileNotFoundError, subprocess.CalledProcessError) as error:
            raise WallpaperError("无法识别该视频；请确认 ffmpeg 已安装且文件可播放") from error
        except (json.JSONDecodeError, KeyError, TypeError, ValueError, subprocess.TimeoutExpired) as error:
            raise WallpaperError("无法读取视频信息") from error
        if width <= 0 or height <= 0:
            raise WallpaperError("视频尺寸无效")
        if (
            width > MAX_SOURCE_DIMENSION
            or height > MAX_SOURCE_DIMENSION
            or width * height > MAX_SOURCE_PIXELS
        ):
            raise WallpaperError("视频分辨率过高")
        if not 0 < duration <= MAX_DURATION_SECONDS:
            raise WallpaperError("视频时长必须在 10 分钟以内")
        return {"width": width, "height": height, "duration": duration}

    @staticmethod
    def _filter(fit: str, *, transpose: bool) -> str:
        if fit == "cover":
            value = (
                "scale=480:320:force_original_aspect_ratio=increase,"
                "crop=480:320"
            )
        else:
            value = (
                "scale=480:320:force_original_aspect_ratio=decrease,"
                "pad=480:320:(ow-iw)/2:(oh-ih)/2:black"
            )
        return f"{value},transpose=clock" if transpose else value

    def _transcode_frames(
        self, source: Path, frames: Path, *, fit: str, fps: int
    ) -> tuple[int, list[Path], int]:
        # q:v is inverse quality. Retry with stronger compression until every
        # WebSocket payload remains below the library's fixed 15 KiB ceiling.
        for quality in (9, 12, 15, 18, 21, 24, 27, 30):
            for old_frame in frames.glob("*.jpg"):
                old_frame.unlink()
            command = [
                self.ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-nostdin",
                "-protocol_whitelist",
                "file,pipe",
                "-format_whitelist",
                SAFE_DEMUXERS,
                "-i",
                str(source),
                "-map",
                "0:v:0",
                "-an",
                "-vf",
                f"fps={fps},{self._filter(fit, transpose=True)}",
                "-c:v",
                "mjpeg",
                "-q:v",
                str(quality),
                "-pix_fmt",
                "yuvj420p",
                "-fps_mode",
                "passthrough",
                str(frames / "%06d.jpg"),
            ]
            try:
                subprocess.run(command, check=True, capture_output=True, timeout=900)
            except FileNotFoundError as error:
                raise WallpaperError("未找到 ffmpeg") from error
            except subprocess.TimeoutExpired as error:
                raise WallpaperError("视频转码超时") from error
            except subprocess.CalledProcessError as error:
                raise WallpaperError("视频转码失败") from error

            frame_files = sorted(frames.glob("*.jpg"))
            if not frame_files:
                raise WallpaperError("视频没有生成可播放帧")
            if len(frame_files) > fps * MAX_PACKAGE_DURATION_SECONDS:
                raise WallpaperError("转码帧数超过 10 分钟支持上限")
            total_bytes = 0
            all_safe = True
            for path in frame_files:
                size = path.stat().st_size
                total_bytes += size
                if size <= 4 or size > MAX_FRAME_BYTES:
                    all_safe = False
                    break
                with path.open("rb") as handle:
                    if handle.read(2) != b"\xff\xd8":
                        all_safe = False
                        break
                    handle.seek(-2, os.SEEK_END)
                    if handle.read(2) != b"\xff\xd9":
                        all_safe = False
                        break
                if total_bytes > MAX_TRANSCODE_BYTES:
                    raise WallpaperError("转码后的动态壁纸超过 128 MB")
            if all_safe:
                return quality, frame_files, total_bytes
        raise WallpaperError("画面细节过高，无法压缩到 ESP32 的安全帧大小")

    def _make_thumbnail(self, source: Path, destination: Path, fit: str) -> None:
        command = [
            self.ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
            "-protocol_whitelist",
            "file,pipe",
            "-format_whitelist",
            SAFE_DEMUXERS,
            "-ss",
            "0",
            "-i",
            str(source),
            "-map",
            "0:v:0",
            "-frames:v",
            "1",
            "-vf",
            self._filter(fit, transpose=False),
            "-q:v",
            "6",
            "-y",
            str(destination),
        ]
        try:
            subprocess.run(command, check=True, capture_output=True, timeout=60)
        except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            raise WallpaperError("无法生成视频预览图") from error
        if not destination.is_file() or destination.stat().st_size <= 4:
            raise WallpaperError("无法生成视频预览图")


def copy_bounded_upload(source: BinaryIO, destination: Path, length: int) -> None:
    """Copy an HTTP request body without ever accepting an unbounded stream."""
    if not isinstance(length, int) or length <= 0 or length > MAX_UPLOAD_BYTES:
        raise WallpaperError("文件必须大于 0 且不超过 800 MB")
    remaining = length
    try:
        with destination.open("xb") as output:
            while remaining:
                chunk = source.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise WallpaperError("上传中断")
                output.write(chunk)
                remaining -= len(chunk)
    except FileExistsError as error:
        raise WallpaperError("上传暂存冲突") from error
    except OSError as error:
        raise WallpaperError("无法保存上传文件") from error
