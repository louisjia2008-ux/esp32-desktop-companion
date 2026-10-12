#!/usr/bin/env python3
"""Claude Code hook for the ESP32 Dev Companion. Standard library only.

Register this one script for SessionStart, UserPromptSubmit, PreToolUse,
PostToolUse, PermissionRequest, Notification, Stop and SessionEnd (see
``bridge.py claude-hooks``). It forwards a whitelisted, shortened subset of
the hook input to the loopback Bridge:

* status events are fire-and-forget and never print anything;
* PermissionRequest opens a device card, polls for the choice, and prints a
  decision only when the device answered. Any failure, timeout or a
  terminal answer leaves stdout empty, so Claude Code's own dialog decides.

Prompts, assistant messages, file contents and full paths are never sent.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import PurePath
import sys
import time
from typing import Any
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

BRIDGE_URL = os.environ.get("DEVCOMPANION_BRIDGE_URL", "http://127.0.0.1:8766").rstrip("/")
DECISION_TIMEOUT = float(os.environ.get("DEVCOMPANION_DECISION_TIMEOUT", "120"))
STATUS_TIMEOUT = 0.3
POLL_INTERVAL = 0.25
STATUS_EVENTS = {"SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse",
                 "Notification", "Stop", "SessionEnd"}
FILE_TOOLS = {"Edit", "Write", "MultiEdit", "NotebookEdit", "Read"}
DENY_MESSAGE = "Denied on the ESP32 companion device"


def _short(value: object, limit: int) -> str:
    text = value if isinstance(value, str) else ""
    return text.encode("utf-8")[:limit].decode("utf-8", "ignore")


def session_key(data: dict[str, Any]) -> str:
    return hashlib.sha256(str(data.get("session_id", "")).encode("utf-8")).hexdigest()[:8]


def project_name(data: dict[str, Any]) -> str:
    return _short(PurePath(str(data.get("cwd") or "")).name, 256) or "claude"


def fingerprint(tool_input: object) -> str:
    """Stable ID of one tool call, shared by PermissionRequest and PostToolUse.
    AskUserQuestion gains an ``answers`` key after answering; ignore it."""
    if isinstance(tool_input, dict):
        tool_input = {k: v for k, v in tool_input.items() if k != "answers"}
    encoded = json.dumps(tool_input, sort_keys=True, ensure_ascii=False, default=str)
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()[:16]


def permission_summary(tool: str, tool_input: dict[str, Any]) -> str:
    if tool == "Bash":
        command = str(tool_input.get("command") or "").strip().splitlines()
        return _short("$ " + command[0], 512) if command else ""
    if tool in FILE_TOOLS:
        path = tool_input.get("file_path") or tool_input.get("notebook_path") or ""
        return _short(PurePath(str(path)).name, 512)
    if tool == "WebFetch":
        return _short(urlsplit(str(tool_input.get("url") or "")).netloc, 512)
    if tool == "WebSearch":
        return _short(tool_input.get("query"), 512)
    return ""


def status_payload(data: dict[str, Any]) -> dict[str, Any] | None:
    event = data.get("hook_event_name")
    if event not in STATUS_EVENTS:
        return None
    payload: dict[str, Any] = {"v": 1, "event": event, "session": session_key(data),
                               "project": project_name(data)}
    if event in {"PreToolUse", "PostToolUse"}:
        payload["tool"] = _short(data.get("tool_name"), 128)
        payload["fingerprint"] = fingerprint(data.get("tool_input"))
    return payload


def request_payload(data: dict[str, Any]) -> dict[str, Any] | None:
    tool = str(data.get("tool_name") or "")
    tool_input = data.get("tool_input") if isinstance(data.get("tool_input"), dict) else {}
    payload: dict[str, Any] = {"v": 1, "session": session_key(data),
                               "project": project_name(data),
                               "fingerprint": fingerprint(tool_input)}
    if tool == "AskUserQuestion":
        questions = []
        for question in tool_input.get("questions") or []:
            if not isinstance(question, dict):
                return None
            options = [o.get("label") for o in question.get("options") or [] if isinstance(o, dict)]
            if not all(isinstance(label, str) for label in options):
                return None
            questions.append({
                "header": _short(question.get("header"), 256),
                "question": _short(question.get("question"), 2048),
                "options": [_short(label, 512) for label in options],
                "multi": bool(question.get("multiSelect")),
            })
        payload.update(kind="question", questions=questions)
    else:
        payload.update(kind="permission", tool=_short(tool, 128),
                       summary=permission_summary(tool, tool_input))
    return payload


def decision_output(data: dict[str, Any], result: dict[str, Any]) -> dict[str, Any] | None:
    """Translate an answered device request into hook output, or None."""
    if result.get("status") != "answered":
        return None
    if data.get("tool_name") == "AskUserQuestion":
        tool_input = dict(data.get("tool_input") or {})
        questions = tool_input.get("questions") or []
        indexes = result.get("answers")
        if not isinstance(indexes, list) or len(indexes) != len(questions):
            return None
        answers = {}
        for question, index in zip(questions, indexes):
            options = question.get("options") or []
            if type(index) is not int or not 0 <= index < len(options):
                return None
            answers[question.get("question")] = options[index].get("label")
        tool_input["answers"] = answers
        decision = {"behavior": "allow", "updatedInput": tool_input}
    elif result.get("behavior") == "allow":
        decision = {"behavior": "allow"}
    elif result.get("behavior") == "deny":
        decision = {"behavior": "deny", "message": DENY_MESSAGE}
    else:
        return None
    return {"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision}}


def _call(method: str, path: str, payload: dict[str, Any] | None, timeout: float) -> dict[str, Any]:
    body = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = Request(BRIDGE_URL + path, data=body, method=method,
                      headers={"Content-Type": "application/json"} if body else {})
    with urlopen(request, timeout=timeout) as response:
        result = json.loads(response.read(16384))
    return result if isinstance(result, dict) else {}


def await_decision(data: dict[str, Any]) -> dict[str, Any] | None:
    payload = request_payload(data)
    if payload is None:
        return None
    opened = _call("POST", "/api/claude/requests", payload, 2.0)
    request_id = opened.get("request_id")
    if opened.get("status") != "pending" or not isinstance(request_id, str):
        return None
    deadline = time.monotonic() + DECISION_TIMEOUT
    failures = 0
    while time.monotonic() < deadline:
        time.sleep(POLL_INTERVAL)
        try:
            result = _call("GET", f"/api/claude/requests/{request_id}", None, 1.0)
            failures = 0
        except Exception:
            failures += 1
            if failures >= 3:
                return None
            continue
        if result.get("status") != "pending":
            return decision_output(data, result)
    return None


def main() -> int:
    try:
        data = json.load(sys.stdin)
        if not isinstance(data, dict):
            return 0
        if data.get("hook_event_name") == "PermissionRequest":
            output = await_decision(data)
            if output is not None:
                print(json.dumps(output, ensure_ascii=False))
            return 0
        payload = status_payload(data)
        if payload is not None:
            _call("POST", "/api/claude/status", payload, STATUS_TIMEOUT)
    except Exception:
        # Never disturb Claude Code: no Bridge, no device, or bad input all
        # mean "do nothing" and the normal terminal flow continues.
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
