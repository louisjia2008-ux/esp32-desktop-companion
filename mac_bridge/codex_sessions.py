"""Bounded Codex status tracker and Claude/Codex display selection. No executor."""
from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field
import re

from claude_sessions import MAX_SESSIONS, SESSION_TTL_SECONDS, utf8_prefix
from codex_hook import EVENTS, TOOL_EVENTS

AGENT_STATUS_V2_CAPABILITY = "agent_status_v2"
PRIORITY = {"waiting": 0, "working": 1, "done": 2, "stopped": 2, "idle": 3}
TERMINAL_VISIBLE_SECONDS = 4.0
HEX16 = re.compile(r"[0-9a-f]{16}")
HEX8 = re.compile(r"[0-9a-f]{8}")
MAX_CALLS = 32
MAX_TRACKED_SESSIONS = 32
MAX_RETIRED = 64
QUESTION_TOOLS = {"request_user_input", "functions.request_user_input"}


@dataclass
class CodexSession:
    key: str
    project: str
    state: str = "idle"
    turn: str = ""
    started: float = 0.0
    elapsed: float = 0.0
    changed: float = 0.0
    updated: float = 0.0
    last_stamp: int = 0
    turn_stamp: int = 0
    tool: str = ""
    calls: OrderedDict = field(default_factory=OrderedDict)
    waiting: dict = field(default_factory=dict)
    retired: OrderedDict = field(default_factory=OrderedDict)
    retired_fingerprints: OrderedDict = field(default_factory=OrderedDict)
    closed: OrderedDict = field(default_factory=OrderedDict)


def _bounded_put(items: OrderedDict, key, value, maximum=MAX_RETIRED):
    items[key] = value
    items.move_to_end(key)
    while len(items) > maximum:
        items.popitem(last=False)


def validate_status(payload: object) -> dict:
    allowed = {"v", "event", "session", "project", "stamp_ns", "origin", "turn", "tool", "call", "fingerprint"}
    if (not isinstance(payload, dict) or set(payload) - allowed or
            type(payload.get("v")) is not int or payload["v"] != 1 or payload.get("event") not in EVENTS):
        raise ValueError("invalid Codex status envelope")
    if not isinstance(payload.get("session"), str) or not HEX8.fullmatch(payload["session"]):
        raise ValueError("invalid session")
    if type(payload.get("stamp_ns")) is not int or not 0 <= payload["stamp_ns"] < 2**63:
        raise ValueError("invalid event timestamp")
    for name, limit in (("project", 256), ("tool", 128)):
        value = payload.get(name, "")
        if not isinstance(value, str) or len(value.encode("utf-8")) > limit or (name == "project" and not value):
            raise ValueError("invalid status text")
    event = payload["event"]
    if event == "SessionStart" and payload.get("origin", "startup") not in {"startup", "resume", "compact"}:
        raise ValueError("invalid session origin")
    for name in ("turn", "call", "fingerprint"):
        required = (name == "turn" and event not in {"SessionStart", "SessionEnd"}) or (
            name == "call" and event in {"PreToolUse", "PostToolUse"}) or (name == "fingerprint" and event in TOOL_EVENTS)
        if name in payload or required:
            if not isinstance(payload.get(name), str) or not HEX16.fullmatch(payload[name]):
                raise ValueError("invalid opaque identifier")
    if event in TOOL_EVENTS and not payload.get("tool"):
        raise ValueError("missing tool")
    return payload


class CodexTracker:
    def __init__(self):
        self.sessions: OrderedDict[str, CodexSession] = OrderedDict()
        self.ended: OrderedDict = OrderedDict()
        self.dirty = False
        self.accepted = 0
        self.ignored = 0

    def record_status(self, payload: object, now: float):
        p = validate_status(payload)
        key, event, stamp = p["session"], p["event"], p["stamp_ns"]
        ended = self.ended.get(key)
        if ended and (stamp <= ended[0] or event not in {"SessionStart", "UserPromptSubmit"}):
            self.ignored += 1
            return
        session = self.sessions.get(key)
        if event == "SessionEnd":
            if session and stamp < session.last_stamp:
                self.ignored += 1
                return
            closed = list(session.closed) if session else []
            if session and session.turn:
                closed.append(session.turn)
            _bounded_put(self.ended, key, (stamp, tuple(closed[-MAX_RETIRED:])))
            if session:
                del self.sessions[key]
                self.dirty = True
            self.accepted += 1
            return
        if session is None:
            if len(self.sessions) >= MAX_TRACKED_SESSIONS:
                victim = min(self.sessions.values(), key=lambda s: (-PRIORITY[s.state], s.updated))
                del self.sessions[victim.key]
            session = CodexSession(key, utf8_prefix(p["project"], 32), updated=now, changed=now)
            if ended:
                for turn in ended[1]: _bounded_put(session.closed, turn, True)
                self.ended.pop(key, None)
            self.sessions[key] = session
        if event == "SessionStart":
            # Resume/compact can occur inside an active turn; never reset it.
            session.updated = now
            session.last_stamp = max(stamp, session.last_stamp)
            self.accepted += 1
            return
        turn = p["turn"]
        if turn in session.closed or (session.turn and session.turn != turn and stamp < session.turn_stamp):
            self.ignored += 1
            return
        if session.turn != turn:
            if session.turn:
                _bounded_put(session.closed, session.turn, True)
            session.turn, session.started = turn, now
            session.calls.clear()
            session.waiting.clear()
            session.retired.clear()
            session.retired_fingerprints.clear()
            session.state, session.changed = "working", now
            session.elapsed, session.tool = 0.0, ""
        session.project = utf8_prefix(p["project"], 32)
        previous = (session.state, session.tool, tuple(session.waiting))
        if event in {"Stop", "Interrupt"}:
            session.state = "done" if event == "Stop" else "stopped"
            session.elapsed = max(0.0, now - session.started)
            session.tool = ""
            session.calls.clear()
            session.waiting.clear()
            _bounded_put(session.closed, turn, True)
        elif event in TOOL_EVENTS:
            tool = utf8_prefix(p["tool"], 32)
            fp = p["fingerprint"]
            call = p.get("call", "")
            if event == "PreToolUse":
                if call not in session.retired:
                    _bounded_put(session.calls, call, (tool, fp, stamp), MAX_CALLS)
            elif event == "PostToolUse":
                session.calls.pop(call, None)
                session.waiting.pop(fp, None)
                _bounded_put(session.retired, call, stamp)
                _bounded_put(session.retired_fingerprints, fp, stamp)
            elif session.retired_fingerprints.get(fp, -1) < stamp:
                if len(session.waiting) < MAX_CALLS:
                    session.waiting[fp] = (tool, stamp)
            questions = [v for v in session.calls.values() if v[0] in QUESTION_TOOLS]
            session.state = "waiting" if session.waiting or questions else "working"
            if session.waiting:
                session.tool = max(session.waiting.values(), key=lambda v: v[1])[0]
            elif session.calls:
                session.tool = max(session.calls.values(), key=lambda v: v[2])[0]
            else:
                session.tool = ""
        else:
            # Duplicate prompt events for an already-observed turn do not
            # reset its timer or erase running calls.
            session.state = "waiting" if session.waiting or any(v[0] in QUESTION_TOOLS for v in session.calls.values()) else "working"
        session.updated = now
        session.last_stamp = max(session.last_stamp, stamp)
        session.turn_stamp = max(session.turn_stamp, stamp)
        if previous != (session.state, session.tool, tuple(session.waiting)):
            session.changed = now
        self.dirty = True
        self.accepted += 1

    def expire(self, now):
        for key, s in list(self.sessions.items()):
            if now - s.updated > SESSION_TTL_SECONDS:
                del self.sessions[key]
                self.dirty = True

    def agent_status(self, now):
        result = []
        for s in self.sessions.values():
            if s.state == "idle" or (s.state in {"done", "stopped"} and now - s.changed >= TERMINAL_VISIBLE_SECONDS):
                continue
            bounded = lambda v: max(0, min(int(v), 0xFFFFFF))
            item = {"id": s.key, "source": "codex", "project": s.project, "state": s.state,
                    "elapsed_s": bounded(s.elapsed if s.state in {"done", "stopped"} else now - s.started),
                    "age_s": bounded(now - s.changed), "pending": min(len(s.waiting) + sum(
                        v[0] in QUESTION_TOOLS for v in s.calls.values()), 255)}
            if s.tool:
                item["tool"] = s.tool
            result.append(item)
        return {"v": 1, "type": "agent_status", "sessions": result}

    def snapshot(self):
        return {"sessions": len(self.sessions), "received_events": self.accepted, "ignored_events": self.ignored}


def merged_agent_status(claude, codex: CodexTracker, now: float) -> dict:
    candidates = [dict(item, source="claude") for item in claude.agent_status(now)["sessions"]
                  if item["state"] != "done" or item["age_s"] < TERMINAL_VISIBLE_SECONDS]
    candidates += codex.agent_status(now)["sessions"]
    activity = {("claude", key): s.updated_at for key, s in claude.sessions.items()}
    activity.update({("codex", key): s.updated for key, s in codex.sessions.items()})
    # age_s describes the state transition (including the terminal fade).
    # Selection uses activity instead, so repeated calls of the same tool count.
    sort_key = lambda s: (PRIORITY[s["state"]], -activity[(s["source"], s["id"])], s["source"], s["id"])
    candidates.sort(key=sort_key)
    # Reserve a slot for each active provider, then fill by urgency/recency.
    selected = []
    for source in ("claude", "codex"):
        match = next((s for s in candidates if s["source"] == source and s["state"] in {"working", "waiting"}), None)
        if match is not None:
            selected.append(match)
    for item in candidates:
        if len(selected) >= MAX_SESSIONS:
            break
        if item not in selected:
            selected.append(item)
    selected.sort(key=sort_key)
    return {"v": 1, "type": "agent_status", "sessions": selected}
