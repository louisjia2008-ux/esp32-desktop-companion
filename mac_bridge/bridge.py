#!/usr/bin/env python3
"""WebSocket Bridge and loopback editor for the ESP32 Dev Companion."""

from __future__ import annotations

import argparse
import asyncio
from collections.abc import Callable, Mapping
import ctypes
from dataclasses import dataclass
import functools
import hashlib
import hmac
import json
import math
import os
import secrets
from pathlib import Path
import struct
import sys
import time
from typing import Any

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from claude_sessions import (
    CLAUDE_CAPABILITY,
    ClaudeBroker,
    ClaudeError,
)
from codex_sessions import AGENT_STATUS_V2_CAPABILITY, CodexTracker, merged_agent_status
from desktop_hub import CAPABILITY as HUB_CAPABILITY, BATCH_CAPABILITY as HUB_BATCH_CAPABILITY, HubStore, HubController, HubError, validate_action
from native_helper import NativeHelper
from codex_usage import CodexUsageCollector
from display_settings import (
    AMBIENT_CAPABILITY,
    CLOCK_INTERVAL_SECONDS,
    DisplaySettingsStore,
    clock_message,
)
from events import (
    EXTENDED_ACTIONS_CAPABILITY,
    MAX_ACTIONS,
    EventError,
    EventLedger,
    identifier,
    needs_extended_actions,
    validate_event,
)
from sd_transfer import SdTransfer
from ota import MAX_FIRMWARE_BYTES, OtaError, OtaTransfer, TERMINAL_STATES, validate_firmware
from wallpaper_cache import MAX_PACKAGE_BYTES
from wallpaper_http import WallpaperEditorService
from wallpaper_library import (
    MAX_FRAME_BYTES,
    NATIVE_HEIGHT,
    NATIVE_WIDTH,
    WALLPAPER_ID_PATTERN,
    WallpaperError,
    WallpaperLibrary,
    WallpaperRecord,
)


PROTOCOL_VERSION = 1
MAX_PACKET_BYTES = 2048
MAX_CAPABILITIES = 16
MAX_CAPABILITY_LENGTH = 32
HOST_STATUS_CAPABILITY = "host_status"
HOST_STATUS_INTERVAL_SECONDS = 5.0
HOST_STATUS_CACHE_MAX_AGE_SECONDS = 15.0
CLIENT_SEND_TIMEOUT_SECONDS = 3.0
CLIENT_CLOSE_TIMEOUT_SECONDS = 2.0
MAX_HOST_COUNTER = 0xFFFFFFFF
FRAME_MAGIC = b"DWP1"
FRAME_VERSION = 1
FRAME_FLAGS = 0
FRAME_HEADER = struct.Struct("<4sBBHHHIIQ")
FRAME_HEADER_BYTES = FRAME_HEADER.size
CLAUDE_TICK_SECONDS = 0.25
AGENT_STATUS_MIN_INTERVAL_SECONDS = 0.5
AGENT_STATUS_REFRESH_SECONDS = 5.0
PAIRING_KEY_BYTES = 16


class ProtocolError(ValueError):
    """Raised when a protocol packet violates the v1 contract."""


def _text_field(
    message: dict[str, Any], name: str, maximum: int, *, allow_empty: bool = False
) -> str:
    value = message.get(name)
    if not isinstance(value, str):
        raise ProtocolError(f"{name} must be a string")
    try:
        size = len(value.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise ProtocolError(f"{name} must be valid UTF-8") from error
    if (not allow_empty and not value) or size > maximum:
        raise ProtocolError(f"{name} has invalid length")
    if any(ord(character) < 0x20 and character != "\t" for character in value):
        raise ProtocolError(f"{name} contains control characters")
    return value


def _wallpaper_id(message: dict[str, Any], name: str = "wallpaper_id") -> str:
    value = _text_field(message, name, 16)
    if WALLPAPER_ID_PATTERN.fullmatch(value) is None:
        raise ProtocolError(f"{name} must be 16 lowercase hexadecimal characters")
    return value


def _capabilities(message: dict[str, Any]) -> frozenset[str]:
    value = message.get("capabilities", [])
    if not isinstance(value, list) or len(value) > MAX_CAPABILITIES:
        raise ProtocolError("capabilities must be a bounded list")
    capabilities: set[str] = set()
    for item in value:
        if isinstance(item, str):
            try:
                item.encode("utf-8")
            except UnicodeEncodeError as error:
                raise ProtocolError("capability must be valid UTF-8") from error
        if (
            not isinstance(item, str)
            or not item
            or len(item.encode("utf-8")) > MAX_CAPABILITY_LENGTH
            or any(ord(character) < 0x20 for character in item)
        ):
            raise ProtocolError("capability has invalid content or length")
        if item in capabilities:
            raise ProtocolError("capabilities must be unique")
        capabilities.add(item)
    return frozenset(capabilities)


def parse_device_message(raw: str | bytes) -> dict[str, Any]:
    """Parse only the message types an ESP32 is allowed to emit."""
    if isinstance(raw, bytes):
        raise ProtocolError("binary messages are not supported from the device")
    if len(raw.encode("utf-8")) > MAX_PACKET_BYTES:
        raise ProtocolError("message exceeds size limit")
    try:
        message = json.loads(raw)
    except (json.JSONDecodeError, RecursionError) as error:
        raise ProtocolError("malformed JSON") from error
    if not isinstance(message, dict):
        raise ProtocolError("message must be a JSON object")
    if type(message.get("v")) is not int or message["v"] != PROTOCOL_VERSION:
        raise ProtocolError("unsupported protocol version")

    message_type = _text_field(message, "type", 24)
    if message_type == "hello":
        _text_field(message, "device", 48)
        _text_field(message, "firmware", 24)
        _capabilities(message)
        width = message.get("width")
        height = message.get("height")
        if (
            not isinstance(width, int)
            or isinstance(width, bool)
            or not isinstance(height, int)
            or isinstance(height, bool)
            or width <= 0
            or height <= 0
            or width > 4096
            or height > 4096
        ):
            raise ProtocolError("invalid display dimensions")
    elif message_type == "heartbeat":
        uptime = message.get("uptime_ms")
        if not isinstance(uptime, int) or isinstance(uptime, bool) or uptime < 0:
            raise ProtocolError("uptime_ms must be a non-negative integer")
    elif message_type == "action":
        if set(message) != {"v", "type", "request_id", "action_id"}:
            raise ProtocolError("unsupported action fields")
        try:
            identifier(message.get("request_id"))
            identifier(message.get("action_id"), 32)
        except EventError as error:
            raise ProtocolError(str(error)) from error
    elif message_type == "hub_action":
        try:
            validate_action(message)
        except HubError as error:
            raise ProtocolError(str(error)) from error
    elif message_type == "auth_response":
        if set(message) != {"v", "type", "mac"}:
            raise ProtocolError("unsupported auth response fields")
        mac = message.get("mac")
        if not isinstance(mac, str) or len(mac) != 64 or any(c not in "0123456789abcdef" for c in mac):
            raise ProtocolError("auth mac must be 64 lowercase hex characters")
    elif message_type == "wallpaper_status":
        _wallpaper_id(message)
        state = _text_field(message, "state", 24)
        if state not in {"selected", "playing", "decode_error"}:
            raise ProtocolError("wallpaper state is not allowed")
        sequence = message.get("sequence")
        if (
            not isinstance(sequence, int)
            or isinstance(sequence, bool)
            or sequence < 0
            or sequence > 0xFFFFFFFF
        ):
            raise ProtocolError("sequence must be an unsigned 32-bit integer")
    elif message_type == "ota_ack":
        if set(message) != {"v", "type", "transfer_id", "state", "offset", "reason"}:
            raise ProtocolError("unsupported OTA ack fields")
        _wallpaper_id(message, "transfer_id")
        if message.get("state") not in {"ready", "progress", "complete", "error", "cancelled"}:
            raise ProtocolError("invalid OTA state")
        _text_field(message, "reason", 48, allow_empty=True)
        if type(message.get("offset")) is not int or not 0 <= message["offset"] <= MAX_FIRMWARE_BYTES:
            raise ProtocolError("invalid OTA offset")
    elif message_type == "ota_boot":
        if set(message) != {"v", "type", "firmware", "state", "image_sha256"}:
            raise ProtocolError("unsupported OTA boot fields")
        _text_field(message, "firmware", 24)
        sha = message.get("image_sha256")
        if (message.get("state") not in {"valid", "pending", "unknown"} or not isinstance(sha,str) or
                len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha)):
            raise ProtocolError("invalid OTA boot identity")
    elif message_type == "wallpaper_sync_ack":
        if set(message) != {"v", "type", "transfer_id", "wallpaper_id", "state",
                           "offset", "bytes", "reason", "sd_capacity_bytes"}:
            raise ProtocolError("unsupported wallpaper sync ack fields")
        _wallpaper_id(message, "transfer_id")
        _wallpaper_id(message)
        state = _text_field(message, "state", 16)
        if state not in {"ready", "progress", "verifying", "complete", "cached", "error", "aborted", "cleared"}:
            raise ProtocolError("invalid wallpaper sync state")
        _text_field(message, "reason", 48, allow_empty=True)
        for key, maximum in (("offset", MAX_PACKAGE_BYTES), ("bytes", MAX_PACKAGE_BYTES),
                             ("sd_capacity_bytes", 1 << 48)):
            if type(message.get(key)) is not int or not 0 <= message[key] <= maximum:
                raise ProtocolError("invalid wallpaper sync counter")
        if message["offset"] > message["bytes"]:
            raise ProtocolError("invalid wallpaper sync offset")
    else:
        raise ProtocolError("device message type is not allowed")
    return message


def notification_message(level: str = "info", event_id: str | None = None) -> dict[str, Any]:
    examples = {
        "info": ("New notification", "A new event arrived from your Mac."),
        "success": ("Build Complete", "The task finished successfully."),
        "warning": ("Attention needed", "Review this warning before continuing."),
        "error": ("Task failed", "The demo task could not finish. No command was executed."),
    }
    if level not in examples:
        raise EventError("unsupported notification level")
    title, message = examples[level]
    return {
        "v": PROTOCOL_VERSION, "type": "notification",
        "event_id": event_id or f"evt-{time.time_ns():x}",
        "level": level, "source": "demo", "title": title, "message": message,
        "auto_dismiss_ms": 4000 if level in {"info", "success"} else 0,
    }


def action_required_message(request_id: str = "demo-001") -> dict[str, Any]:
    return {
        "v": PROTOCOL_VERSION, "type": "action_required", "request_id": request_id,
        "source": "demo", "title": "Choose a plan",
        "message": "How should the task continue?",
        "actions": [
            {"id": "minimal", "label": "Minimal fix"},
            {"id": "refactor", "label": "Refactor"},
            {"id": "cancel", "label": "Cancel"},
        ],
    }


def action_ack_message(request_id: str, action_id: str, accepted: bool) -> dict[str, Any]:
    return {"v": 1, "type": "action_ack", "request_id": request_id,
            "action_id": action_id, "accepted": accepted}


def status_message(state: str) -> dict[str, Any]:
    if state == "success":
        return {
            "v": PROTOCOL_VERSION,
            "type": "status",
            "state": "success",
            "title": "SUCCESS",
            "message": "The demo action completed.",
        }
    return {
        "v": PROTOCOL_VERSION,
        "type": "status",
        "state": "error",
        "title": "ERROR",
        "message": "The demo action failed.",
    }


def dismiss_message(request_id: str = "demo-001") -> dict[str, Any]:
    return {
        "v": PROTOCOL_VERSION,
        "type": "dismiss",
        "request_id": request_id,
    }


def _utf8_prefix(value: str, maximum_bytes: int) -> str:
    encoded = value.encode("utf-8")
    if len(encoded) <= maximum_bytes:
        return value
    return encoded[:maximum_bytes].decode("utf-8", errors="ignore")


def video_control_message(record: WallpaperRecord) -> dict[str, Any]:
    return {
        "v": PROTOCOL_VERSION,
        "type": "video_wallpaper",
        "wallpaper_id": record.wallpaper_id,
        "name": _utf8_prefix(record.name, 48),
        "width": NATIVE_WIDTH,
        "height": NATIVE_HEIGHT,
        "fps": record.fps,
        "frame_count": record.frame_count,
    }


def build_video_frame(wallpaper_id: str, sequence: int, jpeg: bytes) -> bytes:
    if WALLPAPER_ID_PATTERN.fullmatch(wallpaper_id) is None:
        raise ProtocolError("invalid wallpaper id")
    if (
        not isinstance(sequence, int)
        or isinstance(sequence, bool)
        or sequence < 0
        or sequence > 0xFFFFFFFF
    ):
        raise ProtocolError("invalid frame sequence")
    if (
        not isinstance(jpeg, bytes)
        or len(jpeg) <= 4
        or len(jpeg) > MAX_FRAME_BYTES
        or not jpeg.startswith(b"\xff\xd8")
        or not jpeg.endswith(b"\xff\xd9")
    ):
        raise ProtocolError("invalid JPEG frame")
    header = FRAME_HEADER.pack(
        FRAME_MAGIC,
        FRAME_VERSION,
        FRAME_FLAGS,
        FRAME_HEADER_BYTES,
        NATIVE_WIDTH,
        NATIVE_HEIGHT,
        sequence,
        len(jpeg),
        int(wallpaper_id, 16),
    )
    return header + jpeg


def encode_message(message: dict[str, Any]) -> str:
    encoded = json.dumps(message, separators=(",", ":"), ensure_ascii=False)
    if len(encoded.encode("utf-8")) > MAX_PACKET_BYTES:
        raise ProtocolError("outbound message exceeds size limit")
    return encoded


def _bounded_integer(value: object, maximum: int, *, minimum: int = 0) -> int | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if value < minimum or value > maximum:
        return None
    return int(round(value))


def host_status_message(metrics: Mapping[str, object]) -> dict[str, Any]:
    """Build a bounded aggregate-only host status packet.

    Invalid metrics are omitted individually. This keeps one unavailable host
    counter from suppressing unrelated, valid dashboard data.
    """

    message: dict[str, Any] = {"v": PROTOCOL_VERSION, "type": "host_status"}
    cpu_percent = _bounded_integer(metrics.get("cpu_percent"), 100)
    if cpu_percent is not None:
        message["cpu_percent"] = cpu_percent

    memory_percent = _bounded_integer(metrics.get("memory_percent"), 100)

    memory_total = _bounded_integer(
        metrics.get("memory_total_mb"), MAX_HOST_COUNTER, minimum=1
    )
    memory_used = _bounded_integer(metrics.get("memory_used_mb"), MAX_HOST_COUNTER)
    if memory_total is not None:
        message["memory_total_mb"] = memory_total
    if memory_used is not None and (
        memory_total is None or memory_used <= memory_total
    ):
        message["memory_used_mb"] = memory_used

    if (
        "memory_percent" in metrics
        and memory_used is not None
        and memory_total is not None
        and memory_used <= memory_total
    ):
        # Match the firmware's integer half-up calculation exactly. Deriving
        # the percentage from the transmitted MB values prevents a rounded
        # float from disagreeing with the absolute values on the device.
        memory_percent = (
            memory_used * 100 + memory_total // 2
        ) // memory_total
    if memory_percent is not None:
        message["memory_percent"] = memory_percent

    uptime = _bounded_integer(metrics.get("uptime_seconds"), MAX_HOST_COUNTER)
    if uptime is not None:
        message["uptime_seconds"] = uptime
    return message


def _has_host_metrics(message: Mapping[str, object]) -> bool:
    return any(
        name in message
        for name in (
            "cpu_percent",
            "memory_percent",
            "memory_used_mb",
            "memory_total_mb",
            "uptime_seconds",
        )
    )


@functools.lru_cache(maxsize=1)
def _darwin_libsystem() -> ctypes.CDLL:
    if sys.platform != "darwin":
        raise OSError("Darwin host statistics are unavailable")
    library = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
    library.mach_host_self.restype = ctypes.c_uint32
    library.host_statistics64.argtypes = [
        ctypes.c_uint32,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    library.host_statistics64.restype = ctypes.c_int
    library.mach_port_deallocate.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    library.mach_port_deallocate.restype = ctypes.c_int
    library.sysctlbyname.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    library.sysctlbyname.restype = ctypes.c_int
    return library


def _darwin_host_statistics(flavor: int) -> tuple[int, ...]:
    library = _darwin_libsystem()
    values = (ctypes.c_uint32 * 64)()
    count = ctypes.c_uint32(len(values))
    # Resolve the task port before acquiring a new host send-right reference;
    # a loader/symbol failure can then never strand that reference.
    task_port = ctypes.c_uint32.in_dll(library, "mach_task_self_").value
    host_port = library.mach_host_self()
    if host_port == 0:
        raise OSError("mach_host_self returned an invalid port")
    try:
        result = library.host_statistics64(
            host_port, flavor, ctypes.byref(values), ctypes.byref(count)
        )
    finally:
        # mach_host_self() creates a send-right reference on every call. Return
        # it even when sampling fails so a long-running Bridge cannot exhaust
        # the task's Mach-port reference count.
        deallocate_result = library.mach_port_deallocate(task_port, host_port)
    if result != 0 or count.value > len(values):
        raise OSError(f"host_statistics64 failed with code {result}")
    if deallocate_result != 0:
        raise OSError(
            f"mach_port_deallocate failed with code {deallocate_result}"
        )
    return tuple(values[: count.value])


def _read_cpu_ticks() -> tuple[int, int, int, int]:
    # HOST_CPU_LOAD_INFO=3; indexes are user, system, idle, and nice ticks.
    values = _darwin_host_statistics(3)
    if len(values) < 4:
        raise OSError("host CPU counters are incomplete")
    return values[0], values[1], values[2], values[3]


def _read_memory_bytes() -> tuple[int, int]:
    # HOST_VM_INFO64=4. Free, inactive, and speculative pages are reclaimable;
    # the remainder includes active, wired, and compressed memory.
    values = _darwin_host_statistics(4)
    if len(values) <= 23:
        raise OSError("host memory counters are incomplete")
    page_size = os.sysconf("SC_PAGE_SIZE")
    total_pages = os.sysconf("SC_PHYS_PAGES")
    if page_size <= 0 or total_pages <= 0:
        raise OSError("host memory size is unavailable")
    reclaimable_pages = values[0] + values[2] + values[23]
    reclaimable_pages = min(reclaimable_pages, total_pages)
    total_bytes = total_pages * page_size
    used_bytes = (total_pages - reclaimable_pages) * page_size
    return used_bytes, total_bytes


def _read_uptime_seconds() -> float:
    class TimeValue(ctypes.Structure):
        _fields_ = [("seconds", ctypes.c_long), ("microseconds", ctypes.c_int)]

    library = _darwin_libsystem()
    boot_time = TimeValue()
    size = ctypes.c_size_t(ctypes.sizeof(boot_time))
    result = library.sysctlbyname(
        b"kern.boottime", ctypes.byref(boot_time), ctypes.byref(size), None, 0
    )
    if result != 0 or boot_time.seconds <= 0:
        error_number = ctypes.get_errno()
        raise OSError(error_number, "kern.boottime is unavailable")
    return time.time() - float(boot_time.seconds) - boot_time.microseconds / 1_000_000


class HostTelemetrySampler:
    """Collect aggregate Mac telemetry without exposing process information."""

    def __init__(
        self,
        *,
        cpu_reader: Callable[[], tuple[int, int, int, int]] | None = None,
        memory_reader: Callable[[], tuple[int, int]] | None = None,
        uptime_reader: Callable[[], float] | None = None,
    ) -> None:
        self._cpu_reader = cpu_reader or _read_cpu_ticks
        self._memory_reader = memory_reader or _read_memory_bytes
        self._uptime_reader = uptime_reader or _read_uptime_seconds
        self._previous_cpu_ticks: tuple[int, int, int, int] | None = None

    def sample(self) -> dict[str, Any]:
        metrics: dict[str, object] = {}
        try:
            ticks = self._cpu_reader()
            if (
                len(ticks) != 4
                or any(isinstance(value, bool) or not isinstance(value, int) for value in ticks)
                or any(value < 0 or value > MAX_HOST_COUNTER for value in ticks)
            ):
                raise ValueError("invalid CPU counters")
            if self._previous_cpu_ticks is not None:
                deltas = tuple(
                    (current - previous) & MAX_HOST_COUNTER
                    for current, previous in zip(ticks, self._previous_cpu_ticks)
                )
                total_delta = sum(deltas)
                if total_delta > 0:
                    busy_delta = deltas[0] + deltas[1] + deltas[3]
                    metrics["cpu_percent"] = busy_delta * 100.0 / total_delta
            self._previous_cpu_ticks = ticks
        except Exception:
            # CPU failure does not suppress memory or uptime.
            pass

        try:
            used_bytes, total_bytes = self._memory_reader()
            if (
                isinstance(used_bytes, bool)
                or isinstance(total_bytes, bool)
                or not isinstance(used_bytes, int)
                or not isinstance(total_bytes, int)
                or used_bytes < 0
                or total_bytes <= 0
                or used_bytes > total_bytes
            ):
                raise ValueError("invalid memory counters")
            metrics["memory_percent"] = used_bytes * 100.0 / total_bytes
            metrics["memory_used_mb"] = used_bytes / (1024 * 1024)
            metrics["memory_total_mb"] = total_bytes / (1024 * 1024)
        except Exception:
            # Memory failure does not suppress CPU or uptime.
            pass

        try:
            metrics["uptime_seconds"] = self._uptime_reader()
        except Exception:
            # Uptime is optional on restricted hosts.
            pass
        return host_status_message(metrics)


def load_or_create_pairing_key(path: Path) -> bytes:
    """The Mac half of device pairing: 16 random bytes kept as 32-hex text,
    readable only by this user. The same text is entered on the ESP32."""
    try:
        text = path.read_text(encoding="ascii").strip()
        key = bytes.fromhex(text)
        if len(key) == PAIRING_KEY_BYTES and text == key.hex():
            return key
        raise ValueError("pairing key file is malformed")
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    key = secrets.token_bytes(PAIRING_KEY_BYTES)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="ascii") as handle:
        handle.write(key.hex() + "\n")
    return key


def pairing_mac(key: bytes, nonce: str) -> str:
    return hmac.new(key, nonce.encode("ascii"), hashlib.sha256).hexdigest()


@dataclass(frozen=True)
class Peer:
    host: str
    port: int

    @classmethod
    def from_connection(cls, connection: ServerConnection) -> "Peer":
        address = connection.remote_address
        if isinstance(address, tuple) and len(address) >= 2:
            return cls(str(address[0]), int(address[1]))
        return cls("unknown", 0)


class Bridge:
    def __init__(
        self,
        library: WallpaperLibrary,
        *,
        host_sampler: HostTelemetrySampler | None = None,
        monotonic_clock: Callable[[], float] | None = None,
        event_ledger: EventLedger | None = None,
        pairing_key: bytes | None = None,
        display_settings: DisplaySettingsStore | None = None,
        hub: HubStore | None = None,
        native: NativeHelper | None = None,
        enable_usage: bool = False,
    ) -> None:
        self.library = library
        self.hub = hub or HubStore()
        self.hub_controller = HubController(self.hub, native)
        self.usage_collector = CodexUsageCollector(self.hub, self._hub_active) if enable_usage else None
        if self.usage_collector:
            self.hub_controller.refresh_usage = self.usage_collector.refresh
        self.hub_task = None
        self.hub_jobs = set()
        self.hub_send_locks = {}
        self.hub_sent_sections = {}
        self.hub_sent_revision = 0
        self.hub_session_signature = ""
        self.hub_control_read_at = 0.0
        self.ota_transfer = None
        self.ota_connection = None
        self.ota_peer = None
        self.device_info = {}
        self.ota_started_at = 0.0
        self.display_settings = display_settings or DisplaySettingsStore(None)
        self.ambient_task: asyncio.Task[None] | None = None
        self.event_ledger = event_ledger or EventLedger()
        # Claude decisions go only to devices that proved the pairing key.
        # Without a key no device can authenticate, so none receives them.
        self.pairing_key = pairing_key
        self.claude = ClaudeBroker()
        self.codex = CodexTracker()
        self.auth_nonces: dict[ServerConnection, str] = {}
        self.authenticated_clients: set[ServerConnection] = set()
        self.claude_task: asyncio.Task[None] | None = None
        self.agent_status_sent_at = -math.inf
        self.clients: set[ServerConnection] = set()
        self.ready_clients: set[ServerConnection] = set()
        self.peers: dict[ServerConnection, Peer] = {}
        self.send_locks: dict[ServerConnection, asyncio.Lock] = {}
        self.client_capabilities: dict[ServerConnection, frozenset[str]] = {}
        self.device_status: dict[ServerConnection, dict[str, Any]] = {}
        self.sd_transfers: dict[ServerConnection, SdTransfer] = {}
        self.sd_tasks: set[asyncio.Task] = set()
        self.streamed_frame_packets = 0
        self.current_request_id = "demo-001"
        self.stop_event = asyncio.Event()
        self.stream_task: asyncio.Task[None] | None = None
        self.stream_stop_event: asyncio.Event | None = None
        self.wallpaper_mutation_lock = asyncio.Lock()
        self.host_status_task: asyncio.Task[None] | None = None
        self.host_sampler = host_sampler or HostTelemetrySampler()
        self.monotonic_clock = monotonic_clock or time.monotonic
        self.hub_epoch_offset=time.time()-self.monotonic_clock()
        self.latest_host_status: dict[str, Any] | None = None
        self.latest_host_status_sampled_at: float | None = None
        self.active_wallpaper_id = self.library.active_id()

    async def start(self) -> None:
        if self.hub_controller.native:
            self.hub_task = asyncio.create_task(self._hub_loop(), name="desktop-hub")
        if self.usage_collector:
            await self.usage_collector.start()
        await self._start_host_status()
        if self.claude_task is None:
            self.claude_task = asyncio.create_task(self._claude_loop(), name="claude-broker")
        if self.ambient_task is None:
            self.ambient_task = asyncio.create_task(self._ambient_loop(), name="ambient-clock")
        if self.active_wallpaper_id is not None:
            await self._start_streamer(self.active_wallpaper_id)

    async def shutdown(self) -> None:
        if self.hub_task:
            self.hub_task.cancel()
            await asyncio.gather(self.hub_task, return_exceptions=True)
            self.hub_task = None
        for job in tuple(self.hub_jobs): job.cancel()
        await asyncio.gather(*self.hub_jobs, return_exceptions=True)
        if self.usage_collector: await self.usage_collector.close()
        if self.hub_controller.native: await self.hub_controller.native.close()
        if self.ota_transfer and self.ota_transfer.task and not self.ota_transfer.task.done():
            self.ota_transfer.task.cancel()
            await asyncio.gather(self.ota_transfer.task, return_exceptions=True)
        for task in (self.claude_task, self.ambient_task):
            if task is not None:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
        self.claude_task = None
        self.ambient_task = None
        await self._stop_sd_transfers()
        await self._stop_streamer()
        await self._stop_host_status()

    async def handle_client(self, connection: ServerConnection) -> None:
        peer = Peer.from_connection(connection)
        self.clients.add(connection)
        self.peers[connection] = peer
        self.send_locks[connection] = asyncio.Lock()
        print(f"ESP32 connected: {peer.host}:{peer.port}", flush=True)
        try:
            async for raw in connection:
                try:
                    message = parse_device_message(raw)
                except ProtocolError as error:
                    print(f"REJECTED peer={peer.host} reason={error}", flush=True)
                    continue
                await self.handle_device_message(connection, message)
        except ConnectionClosed:
            pass
        finally:
            self._discard_client(connection)
            # Numeric protocol codes are enough to distinguish peer errors,
            # server timeouts, and transport loss without logging payloads.
            received_code = getattr(getattr(connection, "close_rcvd", None), "code", None)
            sent_code = getattr(getattr(connection, "close_sent", None), "code", None)
            print(
                f"ESP32 disconnected: {peer.host}:{peer.port} "
                f"received_code={received_code} sent_code={sent_code}",
                flush=True,
            )

    async def handle_device_message(
        self, connection: ServerConnection, message: dict[str, Any]
    ) -> None:
        message_type = message["type"]
        if message_type == "hello":
            capabilities = _capabilities(message)
            self.client_capabilities[connection] = capabilities
            nonce = message.get("ota_nonce", "")
            if not isinstance(nonce,str) or len(nonce) != 32 or any(c not in "0123456789abcdef" for c in nonce):
                nonce = ""
            self.device_info[connection] = {"firmware": message["firmware"], "ota_nonce": nonce}
            print(
                "HELLO "
                f"device={message['device']} "
                f"size={message['width']}x{message['height']} "
                f"firmware={message['firmware']}",
                flush=True,
            )
            async with self.wallpaper_mutation_lock:
                if self.active_wallpaper_id is not None:
                    try:
                        record = await asyncio.to_thread(
                            self.library.get, self.active_wallpaper_id
                        )
                        if "sd_wallpaper" in capabilities:
                            await self._start_sd_transfer(connection, record.wallpaper_id)
                        else:
                            await self._send(connection, encode_message(video_control_message(record)))
                    except (WallpaperError, ConnectionClosed):
                        self._discard_client(connection)
                        return
            self.ready_clients.add(connection)
            if AMBIENT_CAPABILITY in capabilities:
                try:
                    await self._send(connection, encode_message(clock_message()))
                    await self._send(connection, encode_message(self.display_settings.message()))
                except (ConnectionClosed, RuntimeError, asyncio.TimeoutError):
                    self._discard_client(connection)
                    return
            if (CLAUDE_CAPABILITY in capabilities or AGENT_STATUS_V2_CAPABILITY in capabilities or HUB_CAPABILITY in capabilities) and self.pairing_key is not None:
                nonce = secrets.token_hex(16)
                self.auth_nonces[connection] = nonce
                try:
                    await self._send(connection, encode_message(
                        {"v": PROTOCOL_VERSION, "type": "auth_challenge", "nonce": nonce}))
                except (ConnectionClosed, RuntimeError, asyncio.TimeoutError):
                    self._discard_client(connection)
                    return
            cached_host_status = self._fresh_cached_host_status()
            if (
                HOST_STATUS_CAPABILITY in capabilities
                and cached_host_status is not None
            ):
                try:
                    await self._send(
                        connection, encode_message(cached_host_status)
                    )
                except (ConnectionClosed, RuntimeError):
                    self._discard_client(connection)
        elif message_type == "heartbeat":
            print(f"HEARTBEAT uptime_ms={message['uptime_ms']}", flush=True)
        elif message_type == "auth_response":
            nonce = self.auth_nonces.pop(connection, None)
            if (nonce is None or self.pairing_key is None or
                    not hmac.compare_digest(message["mac"], pairing_mac(self.pairing_key, nonce))):
                print("AUTH_FAILED", flush=True)
                return
            self.authenticated_clients.add(connection)
            print("AUTH_OK claude_companion=true", flush=True)
            try:
                await self._send(connection, encode_message(
                    self._agent_packet(connection, self.monotonic_clock())))
                self._hub_job(self._send_hub(connection))
                if CLAUDE_CAPABILITY in self.client_capabilities.get(connection, ()):
                    for card in self.claude.pending_cards():
                        await self._send(connection, encode_message(card))
            except (ConnectionClosed, RuntimeError, asyncio.TimeoutError):
                self._discard_client(connection)
        elif message_type == "hub_action":
            if connection not in self.authenticated_clients or HUB_CAPABILITY not in self.client_capabilities.get(connection, ()):
                return
            if message['action'] in {'volume_set','mute_set','audio_select','bluetooth_connect','bluetooth_disconnect'}:
                print(f"HUB_ACTION_RECEIVED action={message['action']} target={message.get('target','')} value={message.get('value','')}",flush=True)
            else:
                print(f"HUB_ACTION_RECEIVED action={message['action']}",flush=True)
            previous=self.hub.operation_lookup(message["operation_id"])
            if previous:
                await self._send(connection,encode_message(previous));return
            if self.ota_busy():
                await self._send(connection, encode_message(self.hub.result(message["operation_id"], "failed", "OTA 操作进行中")))
                return
            if message["action"] == "operation_read":
                result = self.hub.operation_lookup(message["target"])
                if result: await self._send(connection, encode_message(result))
                await self._send_hub(connection)
                return
            if message["action"] == "usage_refresh" and self.usage_collector:
                self.usage_collector.usage_page()
            await self._send(connection, encode_message({"v":1,"type":"hub_result","operation_id":message["operation_id"],"state":"running","reason":"操作进行中"}))
            self._hub_job(self._hub_device_action(connection, message))
        elif message_type == "action":
            request_id = message["request_id"]
            action_id = message["action_id"]
            if connection not in self.ready_clients:
                print("REJECTED reason=action_before_hello", flush=True)
                return
            if request_id.startswith("cc-"):
                await self._claude_choice(connection, request_id, action_id)
                return
            try:
                accepted, replay = self.event_ledger.choose(request_id, action_id)
            except (OSError, EventError):
                print("ACTION_NOT_CONFIRMED reason=ledger_unavailable", flush=True)
                return
            print("ACTION RECEIVED", flush=True)
            print(f"request_id={request_id}", flush=True)
            print(f"action_id={action_id}", flush=True)
            print(f"ACTION_RESULT accepted={accepted} replay={replay}", flush=True)
            try:
                await self._send(connection, encode_message(action_ack_message(request_id, action_id, accepted)))
                print(f"ACTION_ACK_SENT request_id={request_id} action_id={action_id} accepted={accepted}", flush=True)
            except (ConnectionClosed, RuntimeError, asyncio.TimeoutError):
                print("ACTION_ACK_NOT_DELIVERED retry_is_idempotent=true", flush=True)
        elif message_type == "wallpaper_status":
            self.device_status[connection] = {
                "wallpaper_id": message["wallpaper_id"],
                "state": message["state"],
                "sequence": message["sequence"],
            }
            print(
                "WALLPAPER_STATUS "
                f"id={message['wallpaper_id']} state={message['state']} "
                f"sequence={message['sequence']}",
                flush=True,
            )
            if (self.active_wallpaper_id is None and message["state"] == "playing" and
                    "sd_wallpaper" in self.client_capabilities.get(connection, ())):
                await self._send(connection, encode_message({
                    "v": 1, "type": "wallpaper_sync", "op": "clear",
                    "transfer_id": secrets.token_hex(8),
                    "wallpaper_id": message["wallpaper_id"],
                }))
        elif message_type == "ota_ack":
            if connection == self.ota_connection and connection in self.authenticated_clients and self.ota_transfer:
                self.ota_transfer.feed(message)
        elif message_type == "ota_boot":
            if connection not in self.authenticated_clients:
                return
            self.device_info.setdefault(connection, {}).update(message)
            transfer = self.ota_transfer
            if (transfer and self.peers.get(connection) and self.peers[connection].host == self.ota_peer
                    and transfer.state in {"rebooting", "validating", "unconfirmed"}):
                if message["firmware"] == transfer.firmware.version and message["image_sha256"] == transfer.firmware.data[-32:].hex():
                    transfer.state = {"valid": "complete", "pending": "validating", "unknown": "unconfirmed"}[message["state"]]
                    transfer.reason = "boot_state_unknown" if message["state"] == "unknown" else ""
                elif (message["state"] == "valid" and message["firmware"] == transfer.old_version
                      and message["image_sha256"] == transfer.old_sha256):
                    transfer.state, transfer.reason = "rolled_back", "previous_firmware_booted"
                else:
                    transfer.state, transfer.reason = "unconfirmed", "unexpected_boot_identity"
        elif message_type == "wallpaper_sync_ack":
            transfer = self.sd_transfers.get(connection)
            if transfer is not None:
                transfer.feed(message)

    def ota_busy(self):
        return self.ota_transfer is not None and self.ota_transfer.state not in TERMINAL_STATES

    async def start_ota(self, data: bytes) -> dict[str, Any]:
        firmware = await asyncio.to_thread(validate_firmware, data)
        async with self.wallpaper_mutation_lock:
            if self.ota_busy():
                raise OtaError("An update is already pending; check its status")
            if self.hub_controller.busy:
                raise OtaError("Mac 操作进行中；结果确认后再发送 OTA")
            targets = [c for c in self.ready_clients if c in self.authenticated_clients
                       and "ota" in self.client_capabilities.get(c, ())
                       and self.device_info.get(c, {}).get("ota_nonce")]
            if len(targets) != 1:
                raise OtaError("Connect exactly one paired OTA-capable ESP32")
            connection = targets[0]
            previous = self.sd_transfers.get(connection)
            if previous:
                previous.stop()
                if previous.task:
                    await asyncio.gather(previous.task, return_exceptions=True)
            async def send(packet):
                await self._send(connection, encode_message(packet) if isinstance(packet, dict) else packet)
            transfer = OtaTransfer(firmware, self.pairing_key,
                                   self.device_info[connection]["ota_nonce"], send)
            transfer.old_version = self.device_info[connection]["firmware"]
            transfer.old_sha256 = self.device_info[connection].get("image_sha256", "")
            self.ota_transfer, self.ota_connection = transfer, connection
            self.ota_peer = self.peers[connection].host
            self.ota_started_at = time.monotonic()
            async def perform():
                try:
                    await transfer.run()
                finally:
                    if transfer.state in TERMINAL_STATES and connection in self.ready_clients and self.active_wallpaper_id:
                        await self._start_sd_transfer(connection, self.active_wallpaper_id)
            transfer.task = asyncio.create_task(perform(), name="firmware-ota")
            return transfer.snapshot()

    async def activate_wallpaper(self, wallpaper_id: str) -> dict[str, Any]:
        if self.ota_busy():
            raise WallpaperError("Firmware update in progress")
        async with self.wallpaper_mutation_lock:
            # Validate before interrupting the current stream. During the swap,
            # expose no active id so a concurrent hello cannot select a record
            # whose frame task is being stopped.
            record = await asyncio.to_thread(self.library.get, wallpaper_id)
            await self._stop_sd_transfers()
            previous_id = self.active_wallpaper_id
            self.active_wallpaper_id = None
            self.device_status.clear()
            await self._stop_streamer()
            try:
                record = await asyncio.to_thread(
                    self.library.set_active, wallpaper_id
                )
            except Exception:
                if previous_id is not None:
                    try:
                        await asyncio.to_thread(self.library.get, previous_id)
                    except WallpaperError:
                        pass
                    else:
                        self.active_wallpaper_id = previous_id
                        await self._start_streamer(previous_id)
                raise

            self.active_wallpaper_id = record.wallpaper_id
            delivered = await self.broadcast(
                video_control_message(record),
                "video_wallpaper",
                quiet_if_empty=True,
                exclude_capability="sd_wallpaper",
            )
            for connection in tuple(self.ready_clients):
                if "sd_wallpaper" in self.client_capabilities.get(connection, ()):
                    await self._start_sd_transfer(connection, record.wallpaper_id)
                    delivered += 1
            await self._start_streamer(record.wallpaper_id)
            return {
                "active_id": record.wallpaper_id,
                "wallpaper": record.public_dict(active=True),
                "delivered_clients": delivered,
            }

    async def delete_wallpaper(self, wallpaper_id: str) -> dict[str, Any]:
        if self.ota_busy():
            raise WallpaperError("Firmware update in progress")
        async with self.wallpaper_mutation_lock:
            # Validation happens before stopping anything. If this is the live
            # record, quiesce its reader before the library atomically renames
            # and removes the backing directory.
            await asyncio.to_thread(self.library.get, wallpaper_id)
            was_bridge_active = self.active_wallpaper_id == wallpaper_id
            if was_bridge_active:
                await self._stop_sd_transfers()
                self.active_wallpaper_id = None
                self.device_status.clear()
                await self._stop_streamer()

            try:
                record, was_library_active, cleanup_complete = (
                    await asyncio.to_thread(self.library.delete, wallpaper_id)
                )
            except Exception:
                if was_bridge_active:
                    try:
                        await asyncio.to_thread(self.library.get, wallpaper_id)
                    except WallpaperError:
                        pass
                    else:
                        self.active_wallpaper_id = wallpaper_id
                        await self._start_streamer(wallpaper_id)
                raise

            was_active = was_bridge_active or was_library_active
            if was_active:
                self.active_wallpaper_id = None
                self.device_status.clear()
                await self.broadcast({
                    "v": 1, "type": "wallpaper_sync", "op": "clear",
                    "transfer_id": secrets.token_hex(8), "wallpaper_id": wallpaper_id,
                }, "wallpaper_sync", quiet_if_empty=True, capability="sd_wallpaper")
            return {
                "deleted_id": record.wallpaper_id,
                "deleted_name": record.name,
                "deleted_active": was_active,
                "active_id": self.active_wallpaper_id,
                "wallpaper_state": "builtin" if was_active else "unchanged",
                "cleanup_pending": not cleanup_complete,
            }

    async def status_snapshot(self) -> dict[str, Any]:
        if (self.ota_transfer and self.ota_transfer.state in {"rebooting", "validating"}
                and time.monotonic() - self.ota_started_at > 360):
            self.ota_transfer.state, self.ota_transfer.reason = "unconfirmed", "boot_confirmation_not_received"
        active = self.active_wallpaper_id
        matching = [
            status
            for connection, status in self.device_status.items()
            if connection in self.ready_clients and status.get("wallpaper_id") == active
        ]
        wallpaper_state = "pending" if active else "builtin"
        if any(item.get("state") == "playing" for item in matching):
            wallpaper_state = "playing"
        elif any(item.get("state") == "decode_error" for item in matching):
            wallpaper_state = "decode_error"
        elif any(item.get("state") == "selected" for item in matching):
            wallpaper_state = "selected"
        transfers = [transfer.snapshot() for connection, transfer in self.sd_transfers.items()
                     if connection in self.ready_clients and transfer.wallpaper_id == active]
        if wallpaper_state != "playing":
            if any(item["state"] == "error" for item in transfers): wallpaper_state = "cache_error"
            elif any(item["state"] == "verifying" for item in transfers): wallpaper_state = "verifying"
            elif any(item["state"] in {"preparing", "uploading"} for item in transfers): wallpaper_state = "uploading"
        sd_clients = sum("sd_wallpaper" in self.client_capabilities.get(c, ()) for c in self.ready_clients)
        mode = "sd_local" if sd_clients == len(self.ready_clients) and sd_clients else "mixed" if sd_clients else "stream"
        return {
            "bridge": "ready",
            "hub": {"revision":self.hub.data["revision"], "paired_clients":len(self._hub_clients()),
                    "collector":self.usage_collector.state if self.usage_collector else "not_provided"},
            "ota": self.ota_transfer.snapshot() if self.ota_transfer else None,
            "devices": [{"firmware": self.device_info.get(c, {}).get("firmware", "unknown"),
                         "paired": c in self.authenticated_clients,
                         "desktop_hub_v1": HUB_CAPABILITY in self.client_capabilities.get(c, ()),
                         "desktop_hub_batch_v1": HUB_BATCH_CAPABILITY in self.client_capabilities.get(c, ()),
                         "boot_state": self.device_info.get(c, {}).get("state", "unknown"),
                         "image_sha256": self.device_info.get(c, {}).get("image_sha256", ""),
                         "agent_status_v2": AGENT_STATUS_V2_CAPABILITY in self.client_capabilities.get(c, ()),
                         "ota": "ota" in self.client_capabilities.get(c, ())}
                        for c in self.ready_clients],
            "connected_clients": len(self.clients),
            "ready_clients": len(self.ready_clients),
            "active_id": active,
            "wallpaper_state": wallpaper_state,
            "events": self.event_ledger.snapshot(),
            "claude": dict(self.claude.snapshot(), paired_clients=len(self._claude_clients())),
            "codex": dict(self.codex.snapshot(), paired_clients=sum(
                AGENT_STATUS_V2_CAPABILITY in self.client_capabilities.get(c, ()) for c in self._agent_clients())),
            "agents": {"visible_sessions": len(merged_agent_status(self.claude, self.codex, self.monotonic_clock())["sessions"]),
                       "paired_clients": len(self._agent_clients())},
            "playback_mode": mode if active else "builtin",
            "sd_transfers": transfers,
            "streamed_frame_packets": self.streamed_frame_packets,
        }

    async def broadcast(
        self,
        message: dict[str, Any],
        label: str,
        *,
        quiet_if_empty: bool = False,
        capability: str | None = None,
        exclude_capability: str | None = None,
    ) -> int:
        if message.get("type") == "action_required" and needs_extended_actions(message):
            if capability is not None and capability != EXTENDED_ACTIONS_CAPABILITY:
                recipients_extra = EXTENDED_ACTIONS_CAPABILITY
            else:
                recipients_extra = None
            capability = capability or EXTENDED_ACTIONS_CAPABILITY
        else:
            recipients_extra = None
        recipients = tuple(
            connection
            for connection in self.ready_clients
            if recipients_extra is None
            or recipients_extra in self.client_capabilities.get(connection, frozenset())
            if capability is None
            or capability in self.client_capabilities.get(connection, frozenset())
            if exclude_capability is None
            or exclude_capability not in self.client_capabilities.get(connection, frozenset())
        )
        if not recipients:
            if not quiet_if_empty:
                print("No ready ESP32 clients connected", flush=True)
            return 0
        if message.get("type") in {"notification", "action_required"}:
            message = validate_event(message)
            self.event_ledger.register(message)
            if message["type"] == "action_required":
                print(f"ACTION_REQUIRED_SENT request_id={message['request_id']} actions=" +
                      ",".join(option["id"] for option in message["actions"]), flush=True)
        encoded = encode_message(message)
        results = await asyncio.gather(
            *(self._send(client, encoded) for client in recipients),
            return_exceptions=True,
        )
        delivered = 0
        for client, result in zip(recipients, results):
            if isinstance(result, Exception):
                self._discard_client(client)
            else:
                delivered += 1
        print(f"SENT type={label} clients={delivered}", flush=True)
        return delivered

    async def publish_event(self, message: dict[str, Any]) -> dict[str, Any]:
        if isinstance(message, dict) and message.get("type") == "dismiss":
            if set(message) != {"v", "type", "request_id"} or type(message.get("v")) is not int or message["v"] != 1:
                raise EventError("unsupported dismiss fields")
            identifier(message.get("request_id"))
            delivered = await self.broadcast(message, "dismiss", capability="interactive_events")
            return {"type": "dismiss", "id": message["request_id"], "delivered_clients": delivered}
        message = validate_event(message)
        delivered = await self.broadcast(message, message["type"], capability="interactive_events")
        return {"type": message["type"],
                "id": message.get("request_id", message.get("event_id")),
                "delivered_clients": delivered}

    # ----- Desktop hub ---------------------------------------------------
    def _hub_clients(self):
        return tuple(c for c in self.ready_clients if c in self.authenticated_clients
                     and HUB_CAPABILITY in self.client_capabilities.get(c, ()))

    def _hub_active(self):
        return (any(t["state"] in {"running","waiting_input"} for t in self.hub.data["tasks"])
                or any(s["state"] in {"working","waiting"} for s in merged_agent_status(
                    self.claude,self.codex,self.monotonic_clock())["sessions"]))

    def _hub_sessions(self):
        now = self.monotonic_clock()
        result = []
        for session in merged_agent_status(self.claude,self.codex,now)["sessions"]:
            state = {"working":"running","waiting":"waiting_input","done":"round_done",
                     "stopped":"round_stopped"}.get(session["state"],"stale")
            result.append({"id":session["source"]+"-"+session["id"],"source":session["source"],
                "title":session["project"].encode()[:48].decode("utf-8","ignore"),"state":state,
                "updated_at":int(self.hub_epoch_offset+(self.codex.sessions[session["id"]].changed if session["source"]=="codex" else self.claude.sessions[session["id"]].changed_at)),"summary":"本轮结束" if state=="round_done" else session.get("tool",""),
                "viewed":False,"has_details":False,"session":True})
        return result

    async def _send_hub(self, connection):
        if connection not in self._hub_clients() or self.ota_busy(): return
        # The Arduino transport consumes one frame per display loop. Avoid
        # queueing entire unchanged lists on every AI session transition, and
        # never interleave two versions of a section for the same device.
        lock=self.hub_send_locks.setdefault(connection,asyncio.Lock())
        async with lock:
            if connection not in self._hub_clients() or self.ota_busy(): return
            grouped={name:[] for name in ("usage","tasks","cards","control")}
            batch_control=HUB_BATCH_CAPABILITY in self.client_capabilities.get(connection,())
            for packet in self.hub.packets(self._hub_sessions(),batch_control=batch_control):grouped[packet["section"]].append(packet)
            sent=self.hub_sent_sections.setdefault(connection,{})
            for section,packets in grouped.items():
                fingerprint=json.dumps([p["items"] for p in packets],sort_keys=True,ensure_ascii=False)
                if sent.get(section)==fingerprint:continue
                for packet in packets:
                    if self.ota_busy():return
                    await self._send(connection,encode_message(packet))
                    await asyncio.sleep(0.15)
                sent[section]=fingerprint

    def _hub_job(self, coroutine):
        task = asyncio.create_task(coroutine, name="hub-operation")
        self.hub_jobs.add(task)
        def completed(job):
            self.hub_jobs.discard(job)
            if not job.cancelled() and job.exception():print(f"HUB_OPERATION_FAILED reason={type(job.exception()).__name__}",flush=True)
        task.add_done_callback(completed)
        return task

    async def _hub_device_action(self, connection, payload):
        result = await self.hub_controller.execute(payload)
        print(f"HUB_ACTION_COMPLETED action={payload['action']} state={result['state']}",flush=True)
        if connection in self._hub_clients():
            try:
                await self._send(connection,encode_message(result))
                await self._send_hub(connection)
            except (ConnectionClosed,RuntimeError,asyncio.TimeoutError): pass

    async def _hub_file_pick(self, operation_id, title, ttl):
        self.hub.result(operation_id,"running","请在 Mac 选择文件")
        try:
            response = await self.hub_controller.native.call("file_pick")
            if response.get("state") != "succeeded": raise HubError(response.get("reason","文件选择失败"))
            self.hub.card({"kind":"file","title":title,"ttl_minutes":ttl},response["path"])
            self.hub.result(operation_id,"succeeded","文件卡已登记")
        except (HubError,OSError,asyncio.TimeoutError) as error:
            self.hub.result(operation_id,"failed",str(error).encode()[:128].decode("utf-8","ignore"))
        except Exception:
            self.hub.result(operation_id,"unconfirmed","文件选择结果未确认；在 Mac 重新选择")

    async def hub_api(self, section, method, payload):
        if section not in {"cards","tasks","usage","actions","control"}: raise HubError("unknown hub section")
        if method == "GET":
            if section == "actions":
                oid = payload.get("operation_id","")
                if oid: return self.hub.operation_lookup(oid) or {"state":"not_provided"}
                return {"operations":list(self.hub.data["operations"].values())[-20:]}
            items=self.hub.sections(self._hub_sessions())[section]
            if section=="tasks": items=[{k:v for k,v in t.items() if k not in {"received_at","terminal_state"}} for t in self.hub.data["tasks"]]+self._hub_sessions()
            return {"revision":self.hub.data["revision"],section:items}
        if not isinstance(payload,dict): raise HubError("JSON object required")
        if section == "cards":
            operation = payload.get("operation","create")
            if operation == "file_pick":
                if self.ota_busy(): raise HubError("OTA 操作进行中")
                if not self.hub_controller.native: raise HubError("native file picker unavailable")
                from desktop_hub import text
                title = text(payload.get("title","待分享文件"),48)
                ttl = payload.get("ttl_minutes",30)
                if type(ttl) is not int or ttl not in {5,30,120}: raise HubError("invalid expiry")
                oid=secrets.token_hex(8)
                self._hub_job(self._hub_file_pick(oid,title,ttl))
                return {"operation_id":oid,"state":"running"}
            if operation in {"pin","clear"}:
                from desktop_hub import ref
                target=ref(payload.get("id"))
                value=payload.get("pinned")
                if operation=="pin" and type(value) is not bool: raise HubError("invalid pin value")
                self.hub.card_action("card_pin" if operation=="pin" else "card_clear",target,value)
                return {"state":"succeeded"}
            return {"card":self.hub.card({k:v for k,v in payload.items() if k!="operation"})}
        if section == "tasks":
            if payload.get("operation") == "viewed":
                self.hub.task_viewed(payload.get("id"));return {"state":"succeeded"}
            return self.hub.task_update(payload)
        if section == "usage": return self.hub.usage_submit(payload)
        if section == "actions":
            action=validate_action(payload)
            old=self.hub.operation_lookup(action["operation_id"])
            if old:return old
            if self.ota_busy():return self.hub.result(action["operation_id"],"failed","OTA 操作进行中")
            if len(self.hub.data["retired_operations"])>=8192:
                return {"operation_id":action["operation_id"],"state":"failed","reason":"操作记录已满；在 Mac 管理历史后再操作"}
            self._hub_job(self.hub_controller.execute(payload))
            return {"operation_id":action["operation_id"],"state":"running"}
        raise HubError("read-only section")

    async def _hub_loop(self):
        while True:
            try:
                now=time.monotonic()
                if now-self.hub_control_read_at>=60 and not self.hub_controller.busy:
                    self.hub_control_read_at=now
                    try:await self.hub_controller.refresh_control()
                    except (HubError,OSError,asyncio.TimeoutError):
                        self.hub.control=[{"kind":"status","state":"error","reason":"Mac 状态读取失败；在 Mac 检查授权"}]
                        self.hub.changed()
                self.hub.expire()
                signature=json.dumps(self._hub_sessions(),sort_keys=True)
                if signature!=self.hub_session_signature:
                    self.hub_session_signature=signature;self.hub.changed()
                if self.hub.data["revision"]!=self.hub_sent_revision and not self.ota_busy():
                    revision=self.hub.data["revision"]
                    for connection in self._hub_clients():
                        try:await self._send_hub(connection)
                        except (ConnectionClosed,RuntimeError,asyncio.TimeoutError):self._discard_client(connection)
                    self.hub_sent_revision=revision
                if self.hub.pending_alerts and self._hub_clients() and not self.ota_busy():
                    notice=self.hub.pending_alerts.pop(0)
                    message={"v":1,"type":"notification","event_id":"hub-"+secrets.token_hex(8),
                             "source":notice["kind"],"level":notice["level"],"title":notice["title"],
                             "message":notice["message"].encode()[:128].decode("utf-8","ignore"),"auto_dismiss_ms":5000}
                    # Only authenticated hub clients receive private task/quota alerts.
                    self.event_ledger.register(validate_event(message))
                    for connection in self._hub_clients():
                        try:await self._send(connection,encode_message(message))
                        except (ConnectionClosed,RuntimeError,asyncio.TimeoutError):self._discard_client(connection)
            except asyncio.CancelledError: raise
            except Exception as error: print(f"HUB_TICK_FAILED reason={type(error).__name__}",flush=True)
            await asyncio.sleep(1)

    # ----- Claude Code companion ------------------------------------------
    def _agent_clients(self) -> tuple[ServerConnection, ...]:
        return tuple(c for c in self.ready_clients if c in self.authenticated_clients and
                     {"agent_status", AGENT_STATUS_V2_CAPABILITY, CLAUDE_CAPABILITY} &
                     self.client_capabilities.get(c, frozenset()))

    def _agent_packet(self, connection, now):
        if AGENT_STATUS_V2_CAPABILITY in self.client_capabilities.get(connection, ()):
            return merged_agent_status(self.claude, self.codex, now)
        return self.claude.agent_status(now)

    async def _send_agent_status(self, now):
        for client in self._agent_clients():
            try:
                await self._send(client, encode_message(self._agent_packet(client, now)))
            except (ConnectionClosed, RuntimeError, asyncio.TimeoutError):
                self._discard_client(client)

    async def codex_api(self, payload):
        self.codex.record_status(payload, self.monotonic_clock())
        return {"ok": True}

    def _claude_clients(self) -> tuple[ServerConnection, ...]:
        return tuple(
            connection for connection in self.ready_clients
            if connection in self.authenticated_clients
            and CLAUDE_CAPABILITY in self.client_capabilities.get(connection, frozenset())
        )

    async def _send_claude(self, message: dict[str, Any]) -> int:
        """Send to paired Claude-capable devices only. Claude requests are not
        written to the persistent event ledger; they die with this process."""
        if message.get("type") == "action_required":
            message = validate_event(message)
        recipients = self._claude_clients()
        if not recipients:
            return 0
        encoded = encode_message(message)
        results = await asyncio.gather(
            *(self._send(client, encoded) for client in recipients), return_exceptions=True)
        delivered = 0
        for client, result in zip(recipients, results):
            if isinstance(result, Exception):
                self._discard_client(client)
            else:
                delivered += 1
        return delivered

    async def _dismiss_claude_cards(self, card_ids: list[str]) -> None:
        for card_id in card_ids:
            delivered = await self._send_claude(dismiss_message(card_id))
            print(f"CLAUDE_CARD_DISMISSED request_id={card_id} clients={delivered}", flush=True)

    async def _claude_choice(
        self, connection: ServerConnection, card_id: str, action_id: str
    ) -> None:
        if connection not in self.authenticated_clients:
            accepted, next_card = False, None
            print("REJECTED reason=unpaired_claude_choice", flush=True)
        else:
            accepted, next_card = self.claude.choose(card_id, action_id, self.monotonic_clock())
            print(f"CLAUDE_CHOICE request_id={card_id} action_id={action_id} accepted={accepted}",
                  flush=True)
        try:
            await self._send(connection, encode_message(
                action_ack_message(card_id, action_id, accepted)))
        except (ConnectionClosed, RuntimeError, asyncio.TimeoutError):
            print("ACTION_ACK_NOT_DELIVERED retry_is_idempotent=true", flush=True)
        if next_card is not None:
            await self._send_claude(next_card)

    async def claude_api(self, operation: str, payload: Any) -> dict[str, Any]:
        """Entry point for the loopback hook API. Raises ClaudeError on bad input."""
        now = self.monotonic_clock()
        if operation == "status":
            await self._dismiss_claude_cards(self.claude.record_status(payload, now))
            return {"ok": True}
        if operation == "open":
            if not self._claude_clients():
                return {"status": "no_device"}
            request = self.claude.open_request(payload, now)
            if request is None:
                return {"status": "unsupported"}
            delivered = await self._send_claude(request.cards[0])
            if delivered == 0:
                self.claude.cancel_request(request.request_id, now)
                return {"status": "no_device"}
            print(f"CLAUDE_REQUEST_OPENED request_id={request.request_id} kind={request.kind} "
                  f"cards={len(request.cards)} clients={delivered}", flush=True)
            return {"status": "pending", "request_id": request.request_id}
        if operation == "poll":
            return self.claude.poll(payload, now)
        raise ClaudeError("unsupported Claude operation")

    async def _claude_tick(self) -> None:
        now = self.monotonic_clock()
        await self._dismiss_claude_cards(self.claude.expire(now))
        self.codex.expire(now)
        if not self._agent_clients():
            return
        since = now - self.agent_status_sent_at
        if (((self.claude.dirty or self.codex.dirty) and since >= AGENT_STATUS_MIN_INTERVAL_SECONDS) or
                since >= AGENT_STATUS_REFRESH_SECONDS):
            self.claude.dirty = False
            self.codex.dirty = False
            self.agent_status_sent_at = now
            await self._send_agent_status(now)

    # ----- clock and brightness -----------------------------------------
    async def update_display_settings(self, settings: Any) -> dict[str, Any]:
        """Validate, persist and push new brightness rules (raises ValueError)."""
        saved = await asyncio.to_thread(self.display_settings.update, settings)
        delivered = await self.broadcast(self.display_settings.message(), "display_settings",
                                         quiet_if_empty=True, capability=AMBIENT_CAPABILITY)
        return {"settings": saved, "delivered_clients": delivered}

    async def _ambient_loop(self) -> None:
        while True:
            await asyncio.sleep(CLOCK_INTERVAL_SECONDS)
            await self.broadcast(clock_message(), "clock", quiet_if_empty=True,
                                 capability=AMBIENT_CAPABILITY)

    async def _claude_loop(self) -> None:
        while True:
            await asyncio.sleep(CLAUDE_TICK_SECONDS)
            try:
                await self._claude_tick()
            except Exception as error:  # keep serving; one bad tick must not stop it
                print(f"CLAUDE_TICK_FAILED reason={type(error).__name__}", flush=True)

    async def _send(self, connection: ServerConnection, payload: str | bytes) -> None:
        lock = self.send_locks.get(connection)
        if lock is None:
            raise RuntimeError("connection is no longer registered")
        async with lock:
            send_task = asyncio.create_task(connection.send(payload))
            try:
                await asyncio.wait_for(
                    asyncio.shield(send_task),
                    timeout=CLIENT_SEND_TIMEOUT_SECONDS,
                )
            except asyncio.TimeoutError:
                await self._close_after_interrupted_send(
                    connection, send_task, "send timeout"
                )
                raise
            except asyncio.CancelledError:
                # websockets explicitly discourages cancelling send(): if the
                # write buffer is full, closing the connection is the only
                # safe way to interrupt it. Shield the actual send and close
                # the peer before propagating cancellation to our caller.
                await self._close_after_interrupted_send(
                    connection, send_task, "send interrupted"
                )
                raise

    async def _close_after_interrupted_send(
        self,
        connection: ServerConnection,
        send_task: asyncio.Task[None],
        reason: str,
    ) -> None:
        self._discard_client(connection)
        try:
            # Connections created by serve() below carry the same bounded
            # close_timeout. Let websockets own that timeout so it can abort
            # the transport itself instead of cancelling close() externally.
            await connection.close(code=1011, reason=reason)
        except (ConnectionClosed, OSError, RuntimeError):
            pass

        if send_task.done():
            await asyncio.gather(send_task, return_exceptions=True)
            return
        try:
            await asyncio.wait_for(
                asyncio.shield(send_task),
                timeout=CLIENT_CLOSE_TIMEOUT_SECONDS,
            )
        except asyncio.TimeoutError:
            # Closing was attempted first. Cancellation here is only a final
            # cleanup for a connection implementation that didn't unblock.
            send_task.cancel()
            await asyncio.gather(send_task, return_exceptions=True)

    def _discard_client(self, connection: ServerConnection) -> None:
        if self.ota_connection == connection and self.ota_transfer:
            self.ota_transfer.disconnected()
        self.device_info.pop(connection, None)
        transfer = self.sd_transfers.pop(connection, None)
        if transfer: transfer.stop()
        self.clients.discard(connection)
        self.ready_clients.discard(connection)
        self.peers.pop(connection, None)
        self.send_locks.pop(connection, None)
        self.client_capabilities.pop(connection, None)
        self.device_status.pop(connection, None)
        self.auth_nonces.pop(connection, None)
        self.authenticated_clients.discard(connection)
        self.hub_send_locks.pop(connection,None)
        self.hub_sent_sections.pop(connection,None)

    async def _start_sd_transfer(self, connection, wallpaper_id):
        if connection == self.ota_connection and self.ota_busy():
            return
        previous = self.sd_transfers.get(connection)
        if previous:
            previous.stop()
            if previous.task: await asyncio.gather(previous.task, return_exceptions=True)
        async def send(packet):
            await self._send(connection, encode_message(packet) if isinstance(packet, dict) else packet)
        transfer = SdTransfer(wallpaper_id, lambda: self.library.sd_package(wallpaper_id), send)
        self.sd_transfers[connection] = transfer
        transfer.task = asyncio.create_task(transfer.run(), name="sd-wallpaper-upload")
        self.sd_tasks.add(transfer.task)
        transfer.task.add_done_callback(self.sd_tasks.discard)

    async def _stop_sd_transfers(self):
        for transfer in tuple(self.sd_transfers.values()): transfer.stop()
        if self.sd_tasks: await asyncio.gather(*tuple(self.sd_tasks), return_exceptions=True)

    async def _start_host_status(self) -> None:
        if self.host_status_task is not None:
            return
        await self._sample_host_status()
        self.host_status_task = asyncio.create_task(
            self._stream_host_status(), name="host-status-stream"
        )

    async def _stop_host_status(self) -> None:
        if self.host_status_task is None:
            return
        self.host_status_task.cancel()
        await asyncio.gather(self.host_status_task, return_exceptions=True)
        self.host_status_task = None

    def _clear_host_status_cache(self) -> None:
        self.latest_host_status = None
        self.latest_host_status_sampled_at = None

    def _fresh_cached_host_status(self) -> dict[str, Any] | None:
        message = self.latest_host_status
        sampled_at = self.latest_host_status_sampled_at
        if message is None or sampled_at is None or not _has_host_metrics(message):
            return None
        try:
            age = self.monotonic_clock() - sampled_at
        except Exception:
            self._clear_host_status_cache()
            return None
        if (
            not math.isfinite(age)
            or age < 0
            or age > HOST_STATUS_CACHE_MAX_AGE_SECONDS
        ):
            self._clear_host_status_cache()
            return None
        return message

    async def _sample_host_status(self) -> dict[str, Any] | None:
        try:
            message = await asyncio.to_thread(self.host_sampler.sample)
        except Exception as error:
            print(
                f"Host telemetry sample unavailable: {type(error).__name__}",
                flush=True,
            )
            self._clear_host_status_cache()
            return None
        if not _has_host_metrics(message):
            self._clear_host_status_cache()
            return None
        try:
            sampled_at = self.monotonic_clock()
        except Exception:
            self._clear_host_status_cache()
            return None
        if not math.isfinite(sampled_at):
            self._clear_host_status_cache()
            return None
        self.latest_host_status = message
        self.latest_host_status_sampled_at = sampled_at
        return message

    async def _stream_host_status(self) -> None:
        try:
            while True:
                await asyncio.sleep(HOST_STATUS_INTERVAL_SECONDS)
                message = await self._sample_host_status()
                if message is None:
                    continue
                await self.broadcast(
                    message,
                    "host_status",
                    quiet_if_empty=True,
                    capability=HOST_STATUS_CAPABILITY,
                )
        except asyncio.CancelledError:
            raise

    async def _start_streamer(self, wallpaper_id: str) -> None:
        if self.stream_task is not None:
            if not self.stream_task.done():
                return
            await asyncio.gather(self.stream_task, return_exceptions=True)
        self.stream_stop_event = asyncio.Event()
        self.stream_task = asyncio.create_task(
            self._stream_wallpaper(wallpaper_id, self.stream_stop_event),
            name="wallpaper-frame-stream",
        )

    async def _stop_streamer(self) -> None:
        if self.stream_task is None:
            return
        task = self.stream_task
        stop_event = self.stream_stop_event
        if stop_event is not None:
            stop_event.set()
        await asyncio.gather(task, return_exceptions=True)
        self.stream_task = None
        self.stream_stop_event = None

    @staticmethod
    async def _wait_for_stream_stop(
        stop_event: asyncio.Event, delay: float
    ) -> bool:
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=delay)
        except asyncio.TimeoutError:
            return False
        return True

    async def _stream_wallpaper(
        self, wallpaper_id: str, stop_event: asyncio.Event
    ) -> None:
        try:
            record = self.library.get(wallpaper_id)
            interval = 1.0 / record.fps
            sequence = 0
            next_due = asyncio.get_running_loop().time()
            while (
                self.active_wallpaper_id == wallpaper_id
                and not stop_event.is_set()
            ):
                recipients = tuple(c for c in self.ready_clients
                                   if "sd_wallpaper" not in self.client_capabilities.get(c, ())
                                   and not (c == self.ota_connection and self.ota_busy()))
                if not recipients:
                    if await self._wait_for_stream_stop(stop_event, 0.2):
                        return
                    next_due = asyncio.get_running_loop().time()
                    continue
                now = asyncio.get_running_loop().time()
                if now < next_due:
                    if await self._wait_for_stream_stop(
                        stop_event, next_due - now
                    ):
                        return
                if (
                    stop_event.is_set()
                    or self.active_wallpaper_id != wallpaper_id
                ):
                    return
                frame_index = sequence % record.frame_count
                try:
                    jpeg = await asyncio.to_thread(
                        self.library.read_frame, wallpaper_id, frame_index
                    )
                    packet = build_video_frame(wallpaper_id, sequence, jpeg)
                except (OSError, WallpaperError, ProtocolError) as error:
                    print(f"Wallpaper stream stopped: {error}", flush=True)
                    return
                if (
                    stop_event.is_set()
                    or self.active_wallpaper_id != wallpaper_id
                ):
                    return
                results = await asyncio.gather(
                    *(self._send(client, packet) for client in recipients),
                    return_exceptions=True,
                )
                for client, result in zip(recipients, results):
                    if isinstance(result, Exception):
                        self._discard_client(client)
                    else:
                        self.streamed_frame_packets += 1
                sequence = (sequence + 1) & 0xFFFFFFFF
                next_due += interval
                now = asyncio.get_running_loop().time()
                if next_due < now - interval:
                    next_due = now + interval
        except asyncio.CancelledError:
            raise
        except WallpaperError as error:
            print(f"Wallpaper stream unavailable: {error}", flush=True)


async def cli_loop(bridge: Bridge) -> None:
    print(
        "1. Send INFO\n2. Send SUCCESS\n3. Send WARNING\n4. Send ERROR\n"
        "5. Send ACTION_REQUIRED\n6. Quit\n"
        "Also: c=clients d=dismiss burst=INFO/SUCCESS/ACTION/ERROR",
        flush=True,
    )
    loop = asyncio.get_running_loop()
    lines: asyncio.Queue[str | None] = asyncio.Queue()

    def stdin_ready() -> None:
        line = sys.stdin.readline()
        lines.put_nowait(line if line else None)

    try:
        loop.add_reader(sys.stdin, stdin_ready)
    except (NotImplementedError, PermissionError):
        print("Interactive demo input unavailable; server remains active", flush=True)
        await bridge.stop_event.wait()
        return

    try:
        while not bridge.stop_event.is_set():
            line = await lines.get()
            if line is None:
                await bridge.stop_event.wait()
                return
            command = line.strip().lower()
            try:
                if command in {"1", "info", "notification", "2", "success", "3", "warning", "4", "error"}:
                    level = {"1": "info", "notification": "info", "2": "success", "3": "warning", "4": "error"}.get(command, command)
                    await bridge.publish_event(notification_message(level))
                elif command in {"5", "action", "action_required"}:
                    bridge.current_request_id = bridge.event_ledger.next_demo_id()
                    await bridge.publish_event(action_required_message(bridge.current_request_id))
                elif command == "burst":
                    await bridge.publish_event(notification_message("info"))
                    await bridge.publish_event(notification_message("success"))
                    bridge.current_request_id = bridge.event_ledger.next_demo_id()
                    await bridge.publish_event(action_required_message(bridge.current_request_id))
                    await bridge.publish_event(notification_message("error"))
                elif command in {"d", "dismiss"}:
                    await bridge.broadcast(dismiss_message(bridge.current_request_id), "dismiss")
                elif command in {"c", "clients"}:
                    print(
                        f"Connected clients: {len(bridge.clients)} "
                        f"ready: {len(bridge.ready_clients)}",
                        flush=True,
                    )
                elif command in {"6", "q", "quit", "exit"}:
                    bridge.stop_event.set()
                elif command:
                    print("Unknown command", flush=True)
            except (EventError, OSError) as error:
                print(f"EVENT_REJECTED reason={type(error).__name__}", flush=True)
    finally:
        loop.remove_reader(sys.stdin)


async def run_bridge(
    host: str,
    port: int,
    enable_cli: bool,
    library_root: Path,
    editor_port: int,
    enable_editor: bool,
) -> None:
    library = WallpaperLibrary(library_root)
    bridge = Bridge(library, event_ledger=EventLedger(library_root.parent / "event-ledger.json"),
                    pairing_key=load_or_create_pairing_key(library_root.parent / "pairing.key"),
                    display_settings=DisplaySettingsStore(
                        library_root.parent / "display-settings.json"))
    bridge.hub = HubStore(library_root.parent / "desktop-hub.json")
    bridge.hub_controller = HubController(bridge.hub, NativeHelper())
    bridge.usage_collector = CodexUsageCollector(bridge.hub, bridge._hub_active)
    bridge.hub_controller.refresh_usage = bridge.usage_collector.refresh
    await bridge.start()
    loop = asyncio.get_running_loop()
    editor: WallpaperEditorService | None = None
    if enable_editor:
        assets = Path(__file__).resolve().parent
        editor = WallpaperEditorService(
            library,
            assets,
            editor_port,
            lambda wallpaper_id: asyncio.run_coroutine_threadsafe(
                bridge.activate_wallpaper(wallpaper_id), loop
            ),
            lambda: asyncio.run_coroutine_threadsafe(bridge.status_snapshot(), loop),
            lambda wallpaper_id: asyncio.run_coroutine_threadsafe(
                bridge.delete_wallpaper(wallpaper_id), loop
            ),
            event=lambda message: asyncio.run_coroutine_threadsafe(bridge.publish_event(message), loop),
            claude=lambda operation, payload: asyncio.run_coroutine_threadsafe(
                bridge.claude_api(operation, payload), loop),
            display=(lambda: bridge.display_settings.message(),
                     lambda settings: asyncio.run_coroutine_threadsafe(
                         bridge.update_display_settings(settings), loop)),
            ota=lambda data: asyncio.run_coroutine_threadsafe(bridge.start_ota(data), loop),
            codex=lambda payload: asyncio.run_coroutine_threadsafe(bridge.codex_api(payload), loop),
            hub=lambda section,method,payload: asyncio.run_coroutine_threadsafe(bridge.hub_api(section,method,payload),loop),
        )
        editor.start()

    print("Dev Companion Bridge", flush=True)
    print(f"ESP32 WebSocket: ws://{host}:{port}", flush=True)
    if editor is not None:
        print(f"Wallpaper editor: http://127.0.0.1:{editor.port}", flush=True)
    async with serve(
        bridge.handle_client,
        host,
        port,
        subprotocols=["arduino"],
        max_size=MAX_PACKET_BYTES,
        ping_interval=20,
        ping_timeout=10,
        close_timeout=CLIENT_CLOSE_TIMEOUT_SECONDS,
    ):
        cli_task = asyncio.create_task(cli_loop(bridge)) if enable_cli else None
        try:
            await bridge.stop_event.wait()
        finally:
            if cli_task is not None:
                cli_task.cancel()
                await asyncio.gather(cli_task, return_exceptions=True)
            await bridge.shutdown()
            if editor is not None:
                await asyncio.to_thread(editor.stop)


CLAUDE_HOOK_EVENTS = ("SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse",
                      "PermissionRequest", "Notification", "Stop", "SessionEnd")


def claude_hooks_settings(hook_path: Path, decision_timeout: int = 120) -> dict[str, Any]:
    """Settings fragment registering claude_hook.py for every needed event."""
    command = f'python3 "{hook_path}"'
    hooks: dict[str, Any] = {}
    for event in CLAUDE_HOOK_EVENTS:
        entry: dict[str, Any] = {"type": "command", "command": command,
                                 "timeout": decision_timeout + 30 if event == "PermissionRequest" else 5}
        group: dict[str, Any] = {"hooks": [entry]}
        if event in {"PreToolUse", "PostToolUse", "PermissionRequest"}:
            group = {"matcher": "*", **group}
        hooks[event] = [group]
    return {"hooks": hooks}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?",
                        choices=("run", "demo", "send-demo", "pair", "claude-hooks", "codex-hooks", "ota"), default="run")
    parser.add_argument("firmware", nargs="?", type=Path,
                        default=Path(__file__).resolve().parents[1] / ".pio/build/deskpet_sensors/firmware.bin")
    parser.add_argument("--event", choices=("info", "success", "warning", "error", "action_required"), default="action_required")
    parser.add_argument("--request-id", default="demo-001")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--editor-port", type=int, default=8766)
    parser.add_argument(
        "--library-root",
        type=Path,
        default=Path(__file__).resolve().parent / ".runtime" / "wallpapers",
    )
    parser.add_argument("--no-cli", action="store_true")
    parser.add_argument("--no-editor", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if not 0 <= args.editor_port <= 65535:
        parser.error("--editor-port must be between 0 and 65535")
    if args.mode == "pair":
        key = load_or_create_pairing_key(args.library_root.parent / "pairing.key")
        print("Bridge pairing code (enter it on the ESP32 setup page):")
        print(key.hex())
        return 0
    if args.mode == "claude-hooks":
        hook = Path(__file__).resolve().parent / "claude_hook.py"
        print("Merge this into ~/.claude/settings.json (it is not applied automatically):")
        print(json.dumps(claude_hooks_settings(hook), indent=2, ensure_ascii=False))
        return 0
    if args.mode == "codex-hooks":
        from codex_hook import hooks_settings
        print(json.dumps(hooks_settings(Path(__file__).resolve().parent / "codex_hook.py"),
                         indent=2, ensure_ascii=False))
        return 0
    if args.mode == "ota":
        from urllib.request import Request, urlopen
        try:
            with args.firmware.open("rb") as handle:
                data = handle.read(MAX_FIRMWARE_BYTES + 1)
            firmware = validate_firmware(data)
            request = Request(f"http://127.0.0.1:{args.editor_port}/api/ota", data=data,
                              headers={"Content-Type": "application/octet-stream"}, method="POST")
            with urlopen(request, timeout=20) as response:
                print(response.read().decode("utf-8"))
            print(f"Offer sent for {firmware.version}. Hold Install on the ESP32 for 1.5 seconds.")
            print(f"Progress: http://127.0.0.1:{args.editor_port}/api/status")
        except (OSError, OtaError) as error:
            print(f"OTA request failed: {error}", file=sys.stderr)
            return 1
        return 0
    if args.mode == "send-demo":
        from urllib.request import Request, urlopen
        packet = action_required_message(args.request_id) if args.event == "action_required" else notification_message(args.event)
        request = Request(f"http://127.0.0.1:{args.editor_port}/api/events",
                          data=encode_message(packet).encode("utf-8"),
                          headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urlopen(request, timeout=12) as response:
                print(response.read().decode("utf-8"))
        except OSError as error:
            print(f"Demo send failed: {error}", file=sys.stderr)
            return 1
        return 0
    try:
        asyncio.run(
            run_bridge(
                args.host,
                args.port,
                not args.no_cli,
                args.library_root,
                args.editor_port,
                not args.no_editor,
            )
        )
    except KeyboardInterrupt:
        pass
    except OSError as error:
        print(f"Bridge failed to start: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
