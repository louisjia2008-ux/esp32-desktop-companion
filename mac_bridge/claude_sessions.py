"""Claude Code session state and device decision broker. No command runner.

The hook script (claude_hook.py) forwards a whitelisted, already-shortened
subset of Claude Code hook input. This module turns it into bounded device
packets and records what the device chose so the waiting hook can read it.
It never executes anything: only the hook converts an answer into a Claude
Code hook decision, and the terminal dialog stays available the whole time.
"""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field
import re
import secrets
from typing import Any

from events import validate_event

CLAUDE_CAPABILITY = "claude_companion"
AGENT_STATUS_CAPABILITY = "agent_status"
MAX_SESSIONS = 4
MAX_PENDING_REQUESTS = 8
MAX_FINISHED_REQUESTS = 32
SESSION_TTL_SECONDS = 6 * 3600.0
DONE_VISIBLE_SECONDS = 10.0
# A waiting hook polls every 250 ms. Missing polls this long means it exited
# (timeout, Ctrl-C, or the terminal answered and the session moved on).
LEASE_SECONDS = 3.0
REQUEST_TTL_SECONDS = 300.0
ALLOW_HOLD_MS = 800

STATUS_EVENTS = frozenset({
    "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse",
    "Notification", "Stop", "SessionEnd",
})
# Events after which every pending request of the session was certainly
# answered somewhere else. PreToolUse is excluded: it races with the
# PermissionRequest of the same call because hooks run as separate processes.
TURN_BOUNDARY_EVENTS = frozenset({"UserPromptSubmit", "Stop", "SessionEnd"})
STATE_PRIORITY = {"waiting": 0, "working": 1, "done": 2, "idle": 3}

SESSION_PATTERN = re.compile(r"[0-9a-f]{8}")
FINGERPRINT_PATTERN = re.compile(r"[0-9a-f]{16}")


class ClaudeError(ValueError):
    """A hook payload violates the bounded loopback contract."""


def utf8_prefix(value: str, maximum_bytes: int) -> str:
    """Single-line text that fits the byte budget, ellipsised when cut."""
    flat = " ".join("".join(" " if ord(c) < 32 or ord(c) == 127 else c
                            for c in value).split())
    encoded = flat.encode("utf-8")
    if len(encoded) <= maximum_bytes:
        return flat
    budget = maximum_bytes - len("…".encode("utf-8"))
    return encoded[:max(budget, 0)].decode("utf-8", "ignore").rstrip() + "…"


def _string(payload: dict[str, Any], key: str, limit: int, *, required: bool = True) -> str:
    value = payload.get(key)
    if value is None and not required:
        return ""
    if not isinstance(value, str) or len(value.encode("utf-8", "surrogatepass")) > limit:
        raise ClaudeError(f"{key} must be a string of at most {limit} bytes")
    try:
        value.encode("utf-8")
    except UnicodeEncodeError as error:
        raise ClaudeError(f"{key} must be valid UTF-8") from error
    return value


def _common(payload: object, allowed: set[str]) -> dict[str, Any]:
    if not isinstance(payload, dict) or type(payload.get("v")) is not int or payload["v"] != 1:
        raise ClaudeError("unsupported payload version")
    unknown = set(payload) - allowed
    if unknown:
        raise ClaudeError("unsupported payload fields")
    session = payload.get("session")
    if not isinstance(session, str) or SESSION_PATTERN.fullmatch(session) is None:
        raise ClaudeError("session must be 8 lowercase hex characters")
    fingerprint = payload.get("fingerprint")
    if fingerprint is not None and (not isinstance(fingerprint, str) or
                                    FINGERPRINT_PATTERN.fullmatch(fingerprint) is None):
        raise ClaudeError("fingerprint must be 16 lowercase hex characters")
    return payload


@dataclass
class Session:
    key: str
    project: str
    state: str = "idle"
    tool: str = ""
    turn_started: float | None = None
    turn_seconds: float = 0.0
    changed_at: float = 0.0
    updated_at: float = 0.0


@dataclass
class Request:
    request_id: str
    session: str
    kind: str
    fingerprint: str
    cards: list[dict[str, Any]]
    created: float
    last_poll: float
    index: int = 0
    status: str = "pending"
    behavior: str = ""
    answers: list[int] = field(default_factory=list)
    chosen: dict[str, str] = field(default_factory=dict)

    @property
    def card_id(self) -> str:
        return self.cards[self.index]["request_id"]


class ClaudeBroker:
    """Bounded, in-memory. Claude requests never outlive the Bridge process:
    a restart ends every waiting hook, which then falls back to the terminal."""

    def __init__(self) -> None:
        self.sessions: OrderedDict[str, Session] = OrderedDict()
        self.requests: OrderedDict[str, Request] = OrderedDict()
        self.dirty = False
        self.counters = {"opened": 0, "answered": 0, "cancelled": 0,
                         "expired": 0, "unsupported": 0, "rejected_choices": 0}

    # ----- hook status -------------------------------------------------
    def record_status(self, payload: object, now: float) -> list[str]:
        """Apply a status event. Returns device card IDs to dismiss."""
        payload = _common(payload, {"v", "event", "session", "project", "tool", "fingerprint"})
        event = payload.get("event")
        if event not in STATUS_EVENTS:
            raise ClaudeError("unsupported hook event")
        project = utf8_prefix(_string(payload, "project", 256), 32) or "claude"
        tool = utf8_prefix(_string(payload, "tool", 128, required=False), 32)
        key = payload["session"]
        dismissed: list[str] = []

        if event == "SessionEnd":
            dismissed = self._cancel(lambda r: r.session == key, "cancelled")
            if self.sessions.pop(key, None) is not None:
                self.dirty = True
            return dismissed

        session = self._session(key, project, now)
        if event in TURN_BOUNDARY_EVENTS:
            dismissed = self._cancel(lambda r: r.session == key, "cancelled")
        elif event == "PostToolUse" and payload.get("fingerprint"):
            fingerprint = payload["fingerprint"]
            dismissed = self._cancel(
                lambda r: r.session == key and r.fingerprint == fingerprint, "cancelled")

        previous = (session.state, session.tool)
        if event == "SessionStart":
            self._set_state(session, "idle", now)
        elif event == "UserPromptSubmit":
            session.turn_started = now
            session.tool = ""
            self._set_state(session, "working", now)
        elif event in {"PreToolUse", "PostToolUse"}:
            if session.state not in {"working", "waiting"}:
                session.turn_started = now
            session.tool = tool
            self._set_state(session, "working", now)
        # Notification is either a permission prompt (PermissionRequest owns
        # that) or an idle reminder after Stop; neither changes the state.
        elif event == "Stop":
            session.turn_seconds = 0.0 if session.turn_started is None else now - session.turn_started
            self._set_state(session, "done", now)
        if self._pending_for(key) and session.state != "waiting":
            self._set_state(session, "waiting", now)
        if (session.state, session.tool) != previous:
            self.dirty = True
        return dismissed

    # ----- device decisions -------------------------------------------
    def open_request(self, payload: object, now: float) -> Request | None:
        """Create a device request, or None when the device cannot answer it
        (the hook then stays silent and the terminal dialog decides)."""
        payload = _common(payload, {"v", "session", "project", "kind", "tool",
                                    "summary", "questions", "fingerprint"})
        kind = payload.get("kind")
        project = utf8_prefix(_string(payload, "project", 256), 32) or "claude"
        if kind == "permission":
            tool = utf8_prefix(_string(payload, "tool", 128), 24) or "tool"
            summary = utf8_prefix(_string(payload, "summary", 512, required=False), 128)
            if "questions" in payload:
                raise ClaudeError("permission requests carry no questions")
            cards = [self._permission_card(project, tool, summary)]
        elif kind == "question":
            questions = payload.get("questions")
            if not isinstance(questions, list) or not 1 <= len(questions) <= 4:
                raise ClaudeError("questions must be a list of one to four items")
            for question in questions:
                if not isinstance(question, dict) or set(question) - {"header", "question", "options", "multi"}:
                    raise ClaudeError("unsupported question fields")
                _string(question, "question", 2048)
                _string(question, "header", 256, required=False)
                options = question.get("options")
                if (not isinstance(options, list) or not 2 <= len(options) <= 4 or
                        not all(isinstance(o, str) and len(o.encode("utf-8")) <= 512 for o in options)):
                    raise ClaudeError("questions need two to four option labels")
                if type(question.get("multi", False)) is not bool:
                    raise ClaudeError("multi must be a boolean")
            if any(q.get("multi") for q in questions):
                self.counters["unsupported"] += 1
                return None
            cards = [self._question_card(project, q, i, len(questions))
                     for i, q in enumerate(questions)]
        else:
            raise ClaudeError("unsupported request kind")
        self._drop_finished()
        if sum(r.status == "pending" for r in self.requests.values()) >= MAX_PENDING_REQUESTS:
            self.counters["unsupported"] += 1
            return None

        request_id = f"cc-{secrets.token_hex(6)}"
        for index, card in enumerate(cards):
            card["request_id"] = f"{request_id}-{index}"
            validate_event(card)  # every card must already be wire-valid
        request = Request(request_id, payload["session"], kind,
                          payload.get("fingerprint") or "", cards, now, now)
        self.requests[request_id] = request
        session = self._session(payload["session"], project, now)
        if session.state != "waiting":
            self._set_state(session, "waiting", now)
            self.dirty = True
        self.counters["opened"] += 1
        return request

    def choose(self, card_id: str, action_id: str, now: float) -> tuple[bool, dict[str, Any] | None]:
        """Record a device choice. Returns (accepted, next card to show).
        A retransmitted identical choice is accepted again without effect."""
        request = self._request_for_card(card_id)
        if request is None:
            self.counters["rejected_choices"] += 1
            return False, None
        if card_id in request.chosen:
            return request.chosen[card_id] == action_id, None
        card = next(c for c in request.cards if c["request_id"] == card_id)
        if (request.status != "pending" or card_id != request.card_id or
                action_id not in {a["id"] for a in card["actions"]}):
            self.counters["rejected_choices"] += 1
            return False, None
        request.chosen[card_id] = action_id
        if request.kind == "permission":
            request.behavior = action_id
            return True, self._finish(request, "answered", now)
        request.answers.append(int(action_id[1:]))
        if request.index + 1 < len(request.cards):
            request.index += 1
            return True, request.cards[request.index]
        return True, self._finish(request, "answered", now)

    def poll(self, request_id: object, now: float) -> dict[str, Any]:
        if not isinstance(request_id, str) or re.fullmatch(r"cc-[0-9a-f]{12}", request_id) is None:
            raise ClaudeError("invalid request identifier")
        request = self.requests.get(request_id)
        if request is None:
            return {"status": "unknown"}
        if request.status == "pending":
            request.last_poll = now
            return {"status": "pending", "question": request.index}
        result: dict[str, Any] = {"status": request.status}
        if request.status == "answered":
            if request.kind == "permission":
                result["behavior"] = request.behavior
            else:
                result["answers"] = list(request.answers)
        return result

    def cancel_request(self, request_id: str, now: float) -> list[str]:
        dismissed = self._cancel(lambda r: r.request_id == request_id, "cancelled")
        self._settle(now)
        return dismissed

    def expire(self, now: float) -> list[str]:
        """Lease/TTL expiry and session cleanup. Returns card IDs to dismiss."""
        dismissed = self._cancel(
            lambda r: now - r.last_poll > LEASE_SECONDS or now - r.created > REQUEST_TTL_SECONDS,
            "expired")
        for key, session in list(self.sessions.items()):
            if now - session.updated_at > SESSION_TTL_SECONDS:
                del self.sessions[key]
                self.dirty = True
        self._settle(now)
        return dismissed

    def pending_cards(self) -> list[dict[str, Any]]:
        return [r.cards[r.index] for r in self.requests.values() if r.status == "pending"]

    # ----- device status ---------------------------------------------
    def agent_status(self, now: float) -> dict[str, Any]:
        visible = [s for s in self.sessions.values()
                   if s.state in {"working", "waiting"} or
                   (s.state == "done" and now - s.changed_at <= DONE_VISIBLE_SECONDS)]
        visible.sort(key=lambda s: (STATE_PRIORITY[s.state], -s.changed_at))
        sessions = []
        for session in visible[:MAX_SESSIONS]:
            elapsed = session.turn_seconds if session.state == "done" else (
                0.0 if session.turn_started is None else now - session.turn_started)
            item: dict[str, Any] = {
                "id": session.key,
                "project": session.project,
                "state": session.state,
                "elapsed_s": max(0, min(int(elapsed), 0xFFFFFF)),
                "age_s": max(0, min(int(now - session.changed_at), 0xFFFFFF)),
                "pending": self._pending_for(session.key),
            }
            if session.tool and session.state in {"working", "waiting"}:
                item["tool"] = session.tool
            sessions.append(item)
        return {"v": 1, "type": "agent_status", "sessions": sessions}

    def snapshot(self) -> dict[str, Any]:
        return {"sessions": len(self.sessions),
                "pending_requests": sum(r.status == "pending" for r in self.requests.values()),
                **self.counters}

    # ----- internals ---------------------------------------------------
    def _session(self, key: str, project: str, now: float) -> Session:
        session = self.sessions.get(key)
        if session is None:
            if len(self.sessions) >= MAX_SESSIONS:
                victim = min(self.sessions.values(),
                             key=lambda s: (s.state in {"working", "waiting"}, s.updated_at))
                self._cancel(lambda r: r.session == victim.key, "cancelled")
                del self.sessions[victim.key]
            session = Session(key, project, changed_at=now)
            self.sessions[key] = session
            self.dirty = True
        session.project = project
        session.updated_at = now
        self.sessions.move_to_end(key)
        return session

    @staticmethod
    def _set_state(session: Session, state: str, now: float) -> None:
        if session.state != state:
            session.state = state
            session.changed_at = now

    def _pending_for(self, session_key: str) -> int:
        return sum(r.session == session_key and r.status == "pending"
                   for r in self.requests.values())

    def _request_for_card(self, card_id: str) -> Request | None:
        match = re.fullmatch(r"(cc-[0-9a-f]{12})-([0-3])", card_id)
        if match is None:
            return None
        request = self.requests.get(match.group(1))
        if request is None or int(match.group(2)) >= len(request.cards):
            return None
        return request

    def _finish(self, request: Request, status: str, now: float) -> None:
        request.status = status
        self.counters[status] += 1
        self.dirty = True
        self._settle(now)

    def _settle(self, now: float) -> None:
        """A session no longer waiting on any request is working again."""
        for session in self.sessions.values():
            if session.state == "waiting" and not self._pending_for(session.key):
                self._set_state(session, "working", now)
                self.dirty = True

    def _cancel(self, predicate, status: str) -> list[str]:
        dismissed = []
        for request in self.requests.values():
            if request.status == "pending" and predicate(request):
                dismissed.append(request.card_id)
                request.status = status
                self.counters[status] += 1
                self.dirty = True
        return dismissed

    def _drop_finished(self) -> None:
        finished = [key for key, r in self.requests.items() if r.status != "pending"]
        for key in finished[:max(0, len(finished) - MAX_FINISHED_REQUESTS)]:
            del self.requests[key]

    @staticmethod
    def _permission_card(project: str, tool: str, summary: str) -> dict[str, Any]:
        return {
            "v": 1, "type": "action_required", "request_id": "",
            "source": project,
            "title": utf8_prefix(f"Claude 请求使用 {tool}", 48),
            "message": summary or utf8_prefix(f"{project} 中的 {tool}", 128),
            "actions": [
                {"id": "allow", "label": "允许", "hold_ms": ALLOW_HOLD_MS},
                {"id": "deny", "label": "拒绝"},
            ],
        }

    @staticmethod
    def _question_card(project: str, question: dict[str, Any], index: int, total: int) -> dict[str, Any]:
        header = utf8_prefix(question.get("header") or "Claude 提问", 40)
        if total > 1:
            header = f"{header} {index + 1}/{total}"
        return {
            "v": 1, "type": "action_required", "request_id": "",
            "source": project,
            "title": utf8_prefix(header, 48),
            "message": utf8_prefix(question["question"], 128) or "?",
            "actions": [{"id": f"o{i}", "label": utf8_prefix(label, 48) or f"选项 {i + 1}"}
                        for i, label in enumerate(question["options"])],
        }


def validate_agent_status(message: object) -> dict[str, Any]:
    """Strict check of the agent_status packet (used by the simulator/tests)."""
    if (not isinstance(message, dict) or set(message) != {"v", "type", "sessions"} or
            message.get("v") != 1 or message.get("type") != "agent_status"):
        raise ClaudeError("invalid agent_status envelope")
    sessions = message["sessions"]
    if not isinstance(sessions, list) or len(sessions) > MAX_SESSIONS:
        raise ClaudeError("invalid agent_status sessions")
    for item in sessions:
        required = {"id", "project", "state", "elapsed_s", "age_s", "pending"}
        if not isinstance(item, dict) or not required <= set(item) <= required | {"tool", "source"}:
            raise ClaudeError("invalid agent_status session fields")
        if "source" in item and item["source"] not in {"claude", "codex"}:
            raise ClaudeError("invalid agent_status source")
        states = set(STATE_PRIORITY) | ({"stopped"} if "source" in item else set())
        if SESSION_PATTERN.fullmatch(str(item["id"])) is None or item["state"] not in states:
            raise ClaudeError("invalid agent_status session identity")
        for key, limit in (("project", 32), ("tool", 32)):
            if key in item and (not isinstance(item[key], str) or
                                len(item[key].encode("utf-8")) > limit):
                raise ClaudeError("invalid agent_status text")
        for key in ("elapsed_s", "age_s", "pending"):
            if type(item[key]) is not int or not 0 <= item[key] <= 0xFFFFFF:
                raise ClaudeError("invalid agent_status counter")
    return message

