#!/usr/bin/env python3
"""Simulate an ESP32 client for a complete Bridge protocol roundtrip."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
from pathlib import Path
import time
from typing import Any

from websockets.asyncio.client import connect

from bridge import (
    FRAME_FLAGS,
    FRAME_HEADER,
    FRAME_HEADER_BYTES,
    FRAME_MAGIC,
    FRAME_VERSION,
    MAX_PACKET_BYTES,
    PROTOCOL_VERSION,
    encode_message,
)
from claude_sessions import validate_agent_status
from events import validate_event
from wallpaper_library import MAX_FRAME_BYTES, NATIVE_HEIGHT, NATIVE_WIDTH


async def heartbeat_loop(connection: Any, started: float) -> None:
    while True:
        await asyncio.sleep(2)
        await connection.send(
            encode_message(
                {
                    "v": PROTOCOL_VERSION,
                    "type": "heartbeat",
                    "uptime_ms": int((time.monotonic() - started) * 1000),
                }
            )
        )


def validate_video_frame(raw: bytes, expected_id: str) -> tuple[int, bytes]:
    if len(raw) < FRAME_HEADER_BYTES + 4:
        raise ValueError("video frame is too short")
    (
        magic,
        version,
        flags,
        header_bytes,
        width,
        height,
        sequence,
        jpeg_bytes,
        wallpaper_key,
    ) = FRAME_HEADER.unpack_from(raw)
    if magic != FRAME_MAGIC or version != FRAME_VERSION or flags != FRAME_FLAGS:
        raise ValueError("video frame header identity is invalid")
    if header_bytes != FRAME_HEADER_BYTES:
        raise ValueError("video frame header length is invalid")
    if width != NATIVE_WIDTH or height != NATIVE_HEIGHT:
        raise ValueError("video frame dimensions are invalid")
    if wallpaper_key != int(expected_id, 16):
        raise ValueError("video frame wallpaper id does not match")
    if jpeg_bytes <= 4 or jpeg_bytes > MAX_FRAME_BYTES:
        raise ValueError("video frame JPEG length is invalid")
    if len(raw) != FRAME_HEADER_BYTES + jpeg_bytes:
        raise ValueError("video frame packet length is invalid")
    jpeg = raw[FRAME_HEADER_BYTES:]
    if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
        raise ValueError("video frame JPEG markers are invalid")
    return sequence, jpeg


def validate_host_status(message: dict[str, Any]) -> dict[str, int]:
    allowed = {
        "v",
        "type",
        "cpu_percent",
        "memory_percent",
        "memory_used_mb",
        "memory_total_mb",
        "uptime_seconds",
    }
    if (
        message.get("v") != PROTOCOL_VERSION
        or isinstance(message.get("v"), bool)
        or message.get("type") != "host_status"
    ):
        raise ValueError("invalid host status identity")
    if set(message) - allowed:
        raise ValueError("host status contains unapproved fields")

    metrics: dict[str, int] = {}
    for name in ("cpu_percent", "memory_percent"):
        if name in message:
            value = message[name]
            if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 100:
                raise ValueError(f"invalid host status field: {name}")
            metrics[name] = value
    for name in ("memory_used_mb", "memory_total_mb", "uptime_seconds"):
        if name in message:
            value = message[name]
            if (
                isinstance(value, bool)
                or not isinstance(value, int)
                or not 0 <= value <= 0xFFFFFFFF
            ):
                raise ValueError(f"invalid host status field: {name}")
            metrics[name] = value
    if not metrics:
        raise ValueError("host status contains no metrics")
    if (
        "memory_used_mb" in metrics
        and "memory_total_mb" in metrics
        and metrics["memory_used_mb"] > metrics["memory_total_mb"]
    ):
        raise ValueError("host status memory relationship is invalid")
    if "memory_total_mb" in metrics and metrics["memory_total_mb"] == 0:
        raise ValueError("host status memory total must be positive")
    if all(
        name in metrics
        for name in ("memory_percent", "memory_used_mb", "memory_total_mb")
    ):
        calculated = (
            metrics["memory_used_mb"] * 100 + metrics["memory_total_mb"] // 2
        ) // metrics["memory_total_mb"]
        if calculated != metrics["memory_percent"]:
            raise ValueError("host status memory percentage is inconsistent")
    return metrics


async def simulate(
    uri: str,
    selected_action: str,
    exit_after_dismiss: bool,
    exit_after_video: bool,
    exit_after_host_status: bool,
) -> None:
    started = time.monotonic()
    async with connect(
        uri,
        subprotocols=["arduino"],
        max_size=max(MAX_PACKET_BYTES, FRAME_HEADER_BYTES + MAX_FRAME_BYTES),
        ping_interval=10,
        ping_timeout=5,
    ) as connection:
        await connection.send(
            encode_message(
                {
                    "v": PROTOCOL_VERSION,
                    "type": "hello",
                    "device": "esp32-companion-simulator",
                    "width": 480,
                    "height": 320,
                    "firmware": "simulator-0.4.0",
                    "capabilities": ["video_wallpaper", "host_status", "interactive_events"],
                }
            )
        )
        print(f"SIMULATOR connected uri={uri}", flush=True)
        heartbeat = asyncio.create_task(heartbeat_loop(connection, started))
        wallpaper_id: str | None = None
        reported_playing = False
        pending_request: str | None = None
        pending_action: str | None = None
        try:
            async for raw in connection:
                if isinstance(raw, bytes):
                    if wallpaper_id is None:
                        raise ValueError("received a video frame before wallpaper control")
                    sequence, jpeg = validate_video_frame(raw, wallpaper_id)
                    if not reported_playing:
                        await connection.send(
                            encode_message(
                                {
                                    "v": PROTOCOL_VERSION,
                                    "type": "wallpaper_status",
                                    "wallpaper_id": wallpaper_id,
                                    "state": "playing",
                                    "sequence": sequence,
                                }
                            )
                        )
                        reported_playing = True
                        print(
                            "SIMULATOR video frame valid "
                            f"id={wallpaper_id} sequence={sequence} bytes={len(jpeg)}",
                            flush=True,
                        )
                        if exit_after_video:
                            print("SIMULATOR video roundtrip complete", flush=True)
                            return
                    continue
                message = json.loads(raw)
                message_type = message.get("type")
                print(f"SIMULATOR received type={message_type}", flush=True)
                if message_type == "video_wallpaper":
                    candidate = message.get("wallpaper_id")
                    if (
                        not isinstance(candidate, str)
                        or len(candidate) != 16
                        or any(character not in "0123456789abcdef" for character in candidate)
                        or message.get("width") != NATIVE_WIDTH
                        or message.get("height") != NATIVE_HEIGHT
                        or message.get("fps") not in {6, 8, 10, 12}
                        or not isinstance(message.get("frame_count"), int)
                        or message["frame_count"] <= 0
                    ):
                        raise ValueError("invalid video wallpaper control")
                    wallpaper_id = candidate
                    reported_playing = False
                    await connection.send(
                        encode_message(
                            {
                                "v": PROTOCOL_VERSION,
                                "type": "wallpaper_status",
                                "wallpaper_id": wallpaper_id,
                                "state": "selected",
                                "sequence": 0,
                            }
                        )
                    )
                    print(f"SIMULATOR selected wallpaper id={wallpaper_id}", flush=True)
                elif message_type == "host_status":
                    metrics = validate_host_status(message)
                    print(
                        "SIMULATOR host status valid "
                        + " ".join(f"{name}={value}" for name, value in metrics.items()),
                        flush=True,
                    )
                    if exit_after_host_status:
                        print("SIMULATOR host status roundtrip complete", flush=True)
                        return
                elif message_type == "action_required":
                    actions = message.get("actions", [])
                    ids = {
                        action.get("id")
                        for action in actions
                        if isinstance(action, dict)
                    }
                    action_id = (
                        selected_action
                        if selected_action in ids
                        else next(iter(ids), None)
                    )
                    if action_id is None:
                        continue
                    pending_request = message.get("request_id")
                    pending_action = action_id
                    await connection.send(
                        encode_message(
                            {
                                "v": PROTOCOL_VERSION,
                                "type": "action",
                                "request_id": message.get("request_id", ""),
                                "action_id": action_id,
                            }
                        )
                    )
                    print(
                        "SIMULATOR sent "
                        f"request_id={message.get('request_id')} "
                        f"action_id={action_id}",
                        flush=True,
                    )
                elif message_type == "action_ack" and exit_after_dismiss:
                    if (message.get("request_id") != pending_request or
                            message.get("action_id") != pending_action or
                            message.get("accepted") is not True):
                        raise ValueError("action ACK rejected or mismatched")
                    print("SIMULATOR ACK roundtrip complete", flush=True)
                    return
        finally:
            heartbeat.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await heartbeat


async def simulate_claude(uri: str, key_hex: str, choose: str, answers: int) -> None:
    """A paired Claude-capable device: authenticate, show agent_status, and
    answer ``answers`` Claude cards. ``choose`` is an action id, or an option
    index for question cards."""
    import hashlib
    import hmac

    key = bytes.fromhex(key_hex.strip())
    async with connect(uri, subprotocols=["arduino"], max_size=MAX_PACKET_BYTES) as connection:
        await connection.send(encode_message({
            "v": PROTOCOL_VERSION, "type": "hello", "device": "esp32-companion-simulator",
            "width": 480, "height": 320, "firmware": "simulator-0.6.0",
            "capabilities": ["interactive_events", "events_v2", "agent_status", "claude_companion"],
        }))
        print(f"SIMULATOR connected uri={uri} mode=claude", flush=True)
        answered = 0
        async for raw in connection:
            if isinstance(raw, bytes):
                continue
            message = json.loads(raw)
            kind = message.get("type")
            if kind == "auth_challenge":
                mac = hmac.new(key, message["nonce"].encode("ascii"), hashlib.sha256).hexdigest()
                await connection.send(encode_message({"v": PROTOCOL_VERSION, "type": "auth_response", "mac": mac}))
            elif kind == "agent_status":
                validate_agent_status(message)
                summary = " ".join(f"{s['project']}:{s['state']}:{s.get('tool', '-')}:{s['elapsed_s']}s"
                                   for s in message["sessions"]) or "(none)"
                print(f"SIMULATOR agent_status {summary}", flush=True)
            elif kind == "action_required" and message["request_id"].startswith("cc-"):
                validate_event(message)
                if choose == "none":
                    print(f"SIMULATOR card {message['request_id']} title={message['title']!r} "
                          "left unanswered", flush=True)
                    continue
                ids = [a["id"] for a in message["actions"]]
                action = choose if choose in ids else (ids[int(choose)] if choose.isdigit() and int(choose) < len(ids) else ids[0])
                labels = " | ".join(f"{a['label']}{'(hold)' if 'hold_ms' in a else ''}" for a in message["actions"])
                print(f"SIMULATOR card {message['request_id']} title={message['title']!r} "
                      f"message={message['message']!r} actions=[{labels}] -> {action}", flush=True)
                await connection.send(encode_message({"v": PROTOCOL_VERSION, "type": "action",
                                                      "request_id": message["request_id"], "action_id": action}))
            elif kind == "action_ack":
                print(f"SIMULATOR ack {message['request_id']} accepted={message['accepted']}", flush=True)
                if message["accepted"]:
                    answered += 1
                    if answered >= answers:
                        print("SIMULATOR claude roundtrip complete", flush=True)
                        return
            elif kind == "dismiss":
                print(f"SIMULATOR dismissed {message['request_id']}", flush=True)
                if choose == "none":
                    print("SIMULATOR card closed without a device answer", flush=True)
                    return


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uri", default="ws://127.0.0.1:8765")
    parser.add_argument("--action-id", default="refactor")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--once-video", action="store_true")
    parser.add_argument("--once-host-status", action="store_true")
    parser.add_argument("--once-claude", action="store_true",
                        help="act as a paired Claude companion and answer cards")
    parser.add_argument("--pairing-key-file", type=Path,
                        default=Path(__file__).resolve().parent / ".runtime" / "pairing.key")
    parser.add_argument("--choose", default="0",
                        help="action id, option index for question cards, or 'none' to leave cards unanswered")
    parser.add_argument("--answers", type=int, default=1)
    args = parser.parse_args()
    if args.once_claude:
        asyncio.run(simulate_claude(args.uri, args.pairing_key_file.read_text(),
                                    args.choose, args.answers))
        return 0
    asyncio.run(
        simulate(
            args.uri,
            args.action_id,
            args.once,
            args.once_video,
            args.once_host_status,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
