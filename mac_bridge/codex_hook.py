#!/usr/bin/env python3
"""Read-only Codex lifecycle observer. Standard library, bounded loopback IO.

No hook decisions, context, commands, prompts or tool results are returned.
"""
from __future__ import annotations

import time
# Capture before slower imports/JSON parsing so handler startup overhead does
# not reorder the event timestamps used by independently scheduled hooks.
_ENTERED_NS = time.clock_gettime_ns(time.CLOCK_MONOTONIC)

import hashlib
import json
import os
from pathlib import PurePath
import shlex
import sys
from urllib.parse import urlsplit
from urllib.request import ProxyHandler, Request, build_opener

EVENTS = ("SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse",
          "PermissionRequest", "Stop", "Interrupt", "SessionEnd")
TOOL_EVENTS = {"PreToolUse", "PostToolUse", "PermissionRequest"}
MAX_INPUT_BYTES = 1024 * 1024
STATUS_TIMEOUT = 0.3


def digest(value: str, size: int = 16) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()[:size]


def short(value: object, limit: int) -> str:
    return value.encode("utf-8")[:limit].decode("utf-8", "ignore") if isinstance(value, str) else ""


def event_stamp_ns() -> int:
    # Python <=3.9 on macOS gives monotonic() a process-local origin. Hooks
    # run in separate processes, so use the kernel clock for comparable IDs.
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def status_payload(data: object, stamp_ns: int | None = None) -> dict | None:
    if not isinstance(data, dict) or data.get("hook_event_name") not in EVENTS:
        return None
    event = data["hook_event_name"]
    session = data.get("session_id")
    if not isinstance(session, str) or not session:
        return None
    result = {"v": 1, "event": event, "session": digest(session, 8),
              "project": short(PurePath(str(data.get("cwd") or "")).name, 256) or "codex",
              "stamp_ns": event_stamp_ns() if stamp_ns is None else stamp_ns}
    if event == "SessionStart":
        result["origin"] = data.get("source") if data.get("source") in {"startup", "resume", "compact"} else "startup"
    if event not in {"SessionStart", "SessionEnd"}:
        turn = data.get("turn_id")
        if not isinstance(turn, str) or not turn:
            return None
        result["turn"] = digest(turn)
    if event in TOOL_EVENTS:
        tool = short(data.get("tool_name"), 128)
        if not tool:
            return None
        result["tool"] = tool
        call = data.get("tool_use_id")
        if event != "PermissionRequest":
            if not isinstance(call, str) or not call:
                return None
            result["call"] = digest(call)
        # PermissionRequest has no tool_use_id. Match its opaque fingerprint
        # to Pre/PostToolUse; never transmit the input itself.
        value = data.get("tool_input")
        if isinstance(value, dict):
            value = {k: v for k, v in value.items() if k != "description"}
        result["fingerprint"] = digest(tool + "\n" + json.dumps(value, sort_keys=True, ensure_ascii=False))
    return result


def bridge_url() -> str:
    value = os.environ.get("DEVCOMPANION_BRIDGE_URL", "http://127.0.0.1:8766").rstrip("/")
    parsed = urlsplit(value)
    if (parsed.scheme != "http" or parsed.hostname not in {"127.0.0.1", "localhost"}
            or parsed.username or parsed.password or parsed.path or parsed.query or parsed.fragment):
        raise ValueError("Bridge must be a loopback HTTP origin")
    return value


def hooks_settings(hook_path) -> dict:
    command = "python3 " + shlex.quote(str(hook_path))
    return {"hooks": {event: [{"hooks": [{"type": "command", "command": command,
                     "timeout": 1, "async": event != "SessionEnd"}]}] for event in EVENTS}}


def main() -> int:
    event = None
    try:
        raw = sys.stdin.buffer.read(MAX_INPUT_BYTES + 1)
        if len(raw) > MAX_INPUT_BYTES:
            return 0
        data = json.loads(raw)
        event = data.get("hook_event_name") if isinstance(data, dict) else None
        payload = status_payload(data, _ENTERED_NS)
        if payload is not None:
            request = Request(bridge_url() + "/api/codex/status",
                              data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
                              headers={"Content-Type": "application/json"}, method="POST")
            # Explicitly avoid forwarding lifecycle metadata through a proxy.
            with build_opener(ProxyHandler({})).open(request, timeout=STATUS_TIMEOUT) as response:
                response.read(1024)
    except Exception:
        pass
    finally:
        # Stop requires JSON rather than plain text. {} has no decisions.
        if event == "Stop":
            print("{}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
