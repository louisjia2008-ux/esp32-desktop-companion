"""Bounded JPEG animation package shared with the SD-card firmware player."""

from dataclasses import dataclass
from pathlib import Path
import os
import struct
import zlib

HEADER = struct.Struct("<4sHHHHHHIIQII")
CHUNK_HEADER = struct.Struct("<4sQII")
HEADER_BYTES = 40
CHUNK_BYTES = 12 * 1024
MAX_PACKAGE_BYTES = 128 * 1024 * 1024
MAX_PACKAGE_DURATION_SECONDS = 600
MAX_JPEG_BYTES = 14 * 1024


class CachePackageError(ValueError):
    pass


@dataclass(frozen=True)
class CachePackage:
    path: Path
    wallpaper_id: str
    size: int
    crc32: int
    frame_count: int
    fps: int


def inspect_package(path: Path, wallpaper_id: str) -> CachePackage:
    size = path.stat().st_size
    if not HEADER_BYTES < size <= MAX_PACKAGE_BYTES:
        raise CachePackageError("Invalid SD wallpaper size")
    with path.open("rb") as handle:
        header = handle.read(HEADER_BYTES)
        fields = HEADER.unpack(header)
        magic, version, header_size, width, height, fps, reserved, count, total, key, data_crc, header_crc = fields
        if (magic != b"DWC1" or version != 1 or header_size != HEADER_BYTES or
                width != 320 or height != 480 or fps not in {6, 8, 10, 12} or
                reserved != 0 or not 1 <= count <= fps * MAX_PACKAGE_DURATION_SECONDS or total != size or
                key != int(wallpaper_id, 16) or zlib.crc32(header[:36]) != header_crc):
            raise CachePackageError("Invalid SD wallpaper header")
        body_crc = 0
        file_crc = zlib.crc32(header)
        for _ in range(count):
            length_bytes = handle.read(4)
            if len(length_bytes) != 4:
                raise CachePackageError("Truncated SD wallpaper index")
            length = struct.unpack("<I", length_bytes)[0]
            if not 4 < length <= MAX_JPEG_BYTES:
                raise CachePackageError("Invalid SD JPEG length")
            jpeg = handle.read(length)
            if len(jpeg) != length or not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
                raise CachePackageError("Invalid SD JPEG data")
            body_crc = zlib.crc32(length_bytes, body_crc)
            body_crc = zlib.crc32(jpeg, body_crc)
            file_crc = zlib.crc32(length_bytes, file_crc)
            file_crc = zlib.crc32(jpeg, file_crc)
        if handle.read(1) or body_crc != data_crc:
            raise CachePackageError("SD wallpaper CRC mismatch")
    return CachePackage(path, wallpaper_id, size, file_crc, count, fps)


def build_package(record, read_frame, destination: Path) -> CachePackage:
    """Called under the library mutation lock; never loads the whole video."""
    if (type(record.fps) is not int or record.fps not in {6, 8, 10, 12} or
            type(record.frame_count) is not int or
            not 1 <= record.frame_count <= record.fps * MAX_PACKAGE_DURATION_SECONDS):
        raise CachePackageError("SD wallpaper exceeds ten-minute frame limit")
    if destination.is_file():
        try:
            package = inspect_package(destination, record.wallpaper_id)
            if package.frame_count == record.frame_count and package.fps == record.fps:
                return package
        except (OSError, ValueError, struct.error):
            pass
    temporary = destination.with_suffix(".part")
    try:
        with temporary.open("wb") as handle:
            os.chmod(temporary, 0o600)
            handle.write(b"\0" * HEADER_BYTES)
            total = HEADER_BYTES
            data_crc = 0
            for index in range(record.frame_count):
                jpeg = read_frame(index)
                if not 4 < len(jpeg) <= MAX_JPEG_BYTES or not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
                    raise CachePackageError("Invalid source JPEG frame")
                length = struct.pack("<I", len(jpeg))
                total += len(length) + len(jpeg)
                if total > MAX_PACKAGE_BYTES:
                    raise CachePackageError("SD wallpaper exceeds package limit")
                handle.write(length)
                handle.write(jpeg)
                data_crc = zlib.crc32(length, data_crc)
                data_crc = zlib.crc32(jpeg, data_crc)
            header = HEADER.pack(b"DWC1", 1, HEADER_BYTES, 320, 480, record.fps, 0,
                                 record.frame_count, total, int(record.wallpaper_id, 16), data_crc, 0)
            header = header[:36] + struct.pack("<I", zlib.crc32(header[:36]))
            handle.seek(0)
            handle.write(header)
            handle.flush()
            os.fsync(handle.fileno())
        package = inspect_package(temporary, record.wallpaper_id)
        os.replace(temporary, destination)
        return CachePackage(destination, package.wallpaper_id, package.size,
                            package.crc32, package.frame_count, package.fps)
    finally:
        temporary.unlink(missing_ok=True)


def chunk_packet(transfer_id: str, offset: int, data: bytes) -> bytes:
    if len(transfer_id) != 16 or any(c not in "0123456789abcdef" for c in transfer_id):
        raise CachePackageError("Invalid transfer id")
    if type(offset) is not int or not 0 <= offset <= MAX_PACKAGE_BYTES or not 0 < len(data) <= CHUNK_BYTES:
        raise CachePackageError("Invalid transfer chunk")
    return CHUNK_HEADER.pack(b"DWS1", int(transfer_id, 16), offset, zlib.crc32(data)) + data
