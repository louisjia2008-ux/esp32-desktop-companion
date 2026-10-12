"""Bounded v1 event validation and idempotent choice records. No command runner."""

from __future__ import annotations

from collections import OrderedDict
import json
import os
from pathlib import Path
import re
from typing import Any

MAX_RECENT = 64
MAX_EVENT_BYTES = 2048
MAX_ACTIONS = 4
# Actions beyond three, or any hold-to-confirm option, need firmware that
# advertises this capability; older firmware rejects such packets.
EXTENDED_ACTIONS_CAPABILITY = "events_v2"
HOLD_MS_RANGE = (300, 2000)
LEVELS = {"info", "success", "warning", "error"}


class EventError(ValueError):
    pass


def text(value: object, limit: int) -> str:
    if not isinstance(value, str):
        raise EventError("event text must be a string")
    try:
        size = len(value.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise EventError("event text must be valid UTF-8") from error
    if (not value or size > limit or
            any(ord(c) < 32 for c in value)):
        raise EventError("invalid or oversized event text")
    return value


def identifier(value: object, limit: int = 48) -> str:
    result = text(value, limit)
    if re.fullmatch(r"[A-Za-z0-9_.:-]+", result) is None:
        raise EventError("invalid event identifier")
    return result


def validate_event(message: object) -> dict[str, Any]:
    if not isinstance(message, dict) or type(message.get("v")) is not int or message["v"] != 1:
        raise EventError("unsupported event version")
    kind = message.get("type")
    if kind == "notification":
        required = {"v", "type", "event_id", "level", "source", "title", "message"}
        if not required <= message.keys() or message.keys() - required - {"auto_dismiss_ms"}:
            raise EventError("unsupported notification fields")
        identifier(message.get("event_id"))
        if not isinstance(message.get("level"), str) or message["level"] not in LEVELS:
            raise EventError("unsupported notification level")
        duration = message.get("auto_dismiss_ms", 4000 if message["level"] in {"info", "success"} else 0)
        if type(duration) is not int or not (duration == 0 or 1000 <= duration <= 600000):
            raise EventError("invalid auto-dismiss duration")
    elif kind == "action_required":
        if set(message) != {"v", "type", "request_id", "source", "title", "message", "actions"}:
            raise EventError("unsupported action request fields")
        identifier(message.get("request_id"))
        actions = message.get("actions")
        if not isinstance(actions, list) or not 1 <= len(actions) <= MAX_ACTIONS:
            raise EventError("action request must have one to four options")
        ids = set()
        for option in actions:
            if (not isinstance(option, dict) or
                    set(option) not in ({"id", "label"}, {"id", "label", "hold_ms"})):
                raise EventError("unsupported action option fields")
            action_id = identifier(option.get("id"), 32)
            text(option.get("label"), 48)
            if "hold_ms" in option:
                hold = option["hold_ms"]
                if type(hold) is not int or not HOLD_MS_RANGE[0] <= hold <= HOLD_MS_RANGE[1]:
                    raise EventError("invalid hold-to-confirm duration")
            if action_id in ids:
                raise EventError("duplicate action identifier")
            ids.add(action_id)
    else:
        raise EventError("unsupported event type")
    text(message.get("source"), 32)
    text(message.get("title"), 48)
    text(message.get("message"), 128)
    encoded = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
    if len(encoded.encode("utf-8")) > MAX_EVENT_BYTES:
        raise EventError("event packet exceeds size limit")
    # Never retain a caller's mutable action list.
    return json.loads(encoded)


def needs_extended_actions(message: dict[str, Any]) -> bool:
    """True when a validated request uses features older firmware rejects."""
    actions = message.get("actions") or []
    return len(actions) > 3 or any("hold_ms" in option for option in actions)


class EventLedger:
    """At most 64 request records, persisted on this Mac for Bridge restart.

    IDs never carry executable meaning. A successful choice records exactly one
    action ID and its ACK; a replay returns that ACK without processing again.
    ESP32 history and deduplication remain RAM-only.
    """

    def __init__(self, path: Path | None = None) -> None:
        self.path = path
        self.records: OrderedDict[str, dict[str, Any]] = OrderedDict()
        self.notifications: OrderedDict[str, dict[str, Any]] = OrderedDict()
        self.action_packets = 0
        self.action_processed = 0
        self.action_replays = 0
        self.duplicate_action_count = 0
        self.processed_ids: OrderedDict[str, str] = OrderedDict()
        if path is not None and path.exists():
            if path.stat().st_size > 256 * 1024:
                raise EventError("event ledger exceeds size limit")
            raw = json.loads(path.read_text())
            if not isinstance(raw, list) or len(raw) > MAX_RECENT:
                raise EventError("invalid event ledger")
            for item in raw:
                if not isinstance(item, dict) or set(item) != {"request", "selected"}:
                    raise EventError("invalid event ledger record")
                request = validate_event(item["request"])
                if request["type"] != "action_required":
                    raise EventError("invalid event ledger request")
                selected = item["selected"]
                if selected is not None and selected not in {a["id"] for a in request["actions"]}:
                    raise EventError("invalid event ledger selection")
                if request["request_id"] in self.records:
                    raise EventError("duplicate event ledger record")
                self.records[request["request_id"]] = {"request": request, "selected": selected}

    def _save(self) -> None:
        if self.path is None:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(".tmp")
        with temporary.open("w") as handle:
            os.chmod(temporary, 0o600)
            json.dump(list(self.records.values()), handle, ensure_ascii=False, separators=(",", ":"))
            handle.flush()
            os.fsync(handle.fileno())
        temporary.replace(self.path)

    def register(self, request: dict[str, Any]) -> bool:
        request = validate_event(request)
        if request["type"] == "notification":
            event_id = request["event_id"]
            previous = self.notifications.get(event_id)
            if previous is not None:
                if previous != request:
                    raise EventError("event ID reused with different content")
                return False
            self.notifications[event_id] = request
            if len(self.notifications) > MAX_RECENT:
                self.notifications.popitem(last=False)
            return True
        request_id = request["request_id"]
        previous = self.records.get(request_id)
        if previous is not None:
            if previous["request"] != request:
                raise EventError("request ID reused with different content")
            return False
        if len(self.records) >= MAX_RECENT:
            victim = next((key for key, value in self.records.items() if value["selected"] is not None), None)
            if victim is None:
                raise EventError("pending request ledger is full")
            del self.records[victim]
        self.records[request_id] = {"request": request, "selected": None}
        self._save()
        return True

    def choose(self, request_id: str, action_id: str) -> tuple[bool, bool]:
        identifier(request_id)
        identifier(action_id, 32)
        self.action_packets += 1
        record = self.records.get(request_id)
        if record is None or action_id not in {a["id"] for a in record["request"]["actions"]}:
            return False, False
        if record["selected"] is not None:
            self.action_replays += 1
            return record["selected"] == action_id, True
        if request_id in self.processed_ids:
            # Detect a lost/mutated in-memory record rather than processing it
            # twice. Ordinary retransmission takes the selected branch above.
            self.duplicate_action_count += 1
            self.action_replays += 1
            return self.processed_ids[request_id] == action_id, True
        record["selected"] = action_id
        try:
            self._save()
        except OSError:
            record["selected"] = None
            raise
        self.action_processed += 1
        self.processed_ids[request_id] = action_id
        if len(self.processed_ids) > MAX_RECENT:
            self.processed_ids.popitem(last=False)
        return True, False

    def next_demo_id(self) -> str:
        maximum = max((int(key[5:]) for key in self.records
                       if key.startswith("demo-") and key[5:].isdigit()), default=0)
        return f"demo-{maximum + 1:03d}"

    def snapshot(self) -> dict[str, Any]:
        return {
            "request_records": len(self.records),
            "action_packets": self.action_packets,
            "action_processed": self.action_processed,
            "action_replays": self.action_replays,
            "duplicate_action_count": self.duplicate_action_count,
            "recent_results": [{"request_id": key, "action_id": item["selected"]}
                               for key, item in list(self.records.items())[-10:]
                               if item["selected"] is not None],
        }
