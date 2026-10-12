from __future__ import annotations

import asyncio
from concurrent.futures import Future
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import importlib.util
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from bridge import Bridge, pairing_mac
from claude_sessions import ClaudeBroker, validate_agent_status
import codex_hook
from codex_sessions import MAX_TRACKED_SESSIONS, CodexTracker, merged_agent_status
from wallpaper_http import WallpaperEditorService
from wallpaper_library import WallpaperLibrary


def event(name="UserPromptSubmit", stamp=1, session="s", turn="t", call="c", tool="Bash", **extra):
    data = dict(hook_event_name=name, session_id=session, turn_id=turn,
                cwd="/private/project/eps32", tool_name=tool, tool_use_id=call,
                tool_input={"command": "private command"})
    data.update(extra)
    return codex_hook.status_payload(data, stamp)


class HookTests(unittest.TestCase):
    def test_hook_processes_share_kernel_clock_origin(self):
        before = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        code = "import codex_hook; print(codex_hook.event_stamp_ns())"
        first = int(subprocess.check_output([sys.executable, "-c", code],
                    cwd=Path(codex_hook.__file__).parent, text=True))
        second = int(subprocess.check_output([sys.executable, "-c", code],
                    cwd=Path(codex_hook.__file__).parent, text=True))
        after = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        self.assertLessEqual(before, first)
        self.assertLess(first, second)
        self.assertLessEqual(second, after)

    def test_install_preserves_other_hooks_is_idempotent_and_backs_up(self):
        spec = importlib.util.spec_from_file_location("installer", Path(__file__).resolve().parents[2] / "scripts/install_codex_hooks.py")
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        existing = {"description":"personal", "hooks":{"PreToolUse":[{"matcher":"Bash", "hooks":[{"type":"command","command":"my-existing-hook"}]}]}}
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)/"hooks.json"
            raw = json.dumps(existing).encode()
            target.write_bytes(raw)
            result = installer.install(target, Path("/tmp/codex_hook.py"))
            self.assertEqual(Path(result["backup"]).read_bytes(), raw)
            self.assertEqual(json.loads(target.read_text())["hooks"]["PreToolUse"][0], existing["hooks"]["PreToolUse"][0])
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            self.assertFalse(installer.install(target, Path("/tmp/codex_hook.py"))["changed"])

    def test_only_bounded_metadata_leaves_hook(self):
        data = event("PreToolUse", session="secret-session", turn="secret-turn", call="secret-call",
                     cwd="/secret/root/中文项目", prompt="secret prompt",
                     tool_response="secret result", last_assistant_message="secret answer")
        self.assertEqual(set(data), {"v", "event", "session", "turn", "call", "project", "tool", "fingerprint", "stamp_ns"})
        self.assertEqual(data["project"], "中文项目")
        self.assertNotIn("secret", json.dumps(data))
        self.assertEqual(len(data["session"]), 8)

    def test_permission_without_call_id_matches_input_fingerprint(self):
        pre = event("PreToolUse")
        permission = event("PermissionRequest", tool_use_id=None,
                           tool_input={"command": "private command", "description": "secret reason"})
        self.assertNotIn("call", permission)
        self.assertEqual(pre["fingerprint"], permission["fingerprint"])

    def test_unknown_missing_identifiers_and_unicode_bounds(self):
        self.assertIsNone(event("SubagentStop"))
        self.assertIsNone(event(session_id=""))
        self.assertIsNone(event(turn_id=""))
        self.assertIsNone(event("PreToolUse", tool_use_id=""))
        self.assertLessEqual(len(event(cwd="/" + "界" * 200)["project"].encode()), 256)

    def test_loopback_only_and_hooks_are_passive(self):
        for url in ("https://127.0.0.1:8766", "http://example.com", "http://user:password@localhost", "http://localhost/path"):
            with patch.dict("os.environ", {"DEVCOMPANION_BRIDGE_URL": url}):
                with self.assertRaises(ValueError): codex_hook.bridge_url()
        settings = codex_hook.hooks_settings(Path("/tmp/path with spaces/codex_hook.py"))
        self.assertEqual(set(settings["hooks"]), set(codex_hook.EVENTS))
        for e, groups in settings["hooks"].items():
            h = groups[0]["hooks"][0]
            self.assertEqual(h["timeout"], 1)
            self.assertEqual(h["async"], e != "SessionEnd")

    def test_bridge_failure_is_silent_and_stop_has_no_decision(self):
        for name, output in (("PreToolUse", ""), ("Stop", "{}\n")):
            raw = json.dumps(dict(hook_event_name=name, session_id="s", turn_id="t",
                                 tool_name="Bash", tool_use_id="c")).encode()
            stdin = type("Input", (), {"buffer": io.BytesIO(raw)})()
            stdout = io.StringIO()
            with patch.object(codex_hook.sys, "stdin", stdin), patch.object(codex_hook.sys, "stdout", stdout), \
                 patch.object(codex_hook, "build_opener", side_effect=OSError("offline")):
                self.assertEqual(codex_hook.main(), 0)
            self.assertEqual(stdout.getvalue(), output)


class TrackerTests(unittest.TestCase):
    def setUp(self): self.tracker = CodexTracker()
    def record(self, name, now, **kw): self.tracker.record_status(event(name, stamp=int(now*1000), **kw), now)
    def current(self, now): return self.tracker.agent_status(now)["sessions"][0]

    def test_parallel_calls_clear_only_matching_call_and_return_to_thinking(self):
        self.record("UserPromptSubmit", 0)
        self.record("PreToolUse", 1, call="a", tool="Bash")
        self.record("PreToolUse", 2, call="b", tool="apply_patch")
        self.record("PostToolUse", 3, call="b", tool="apply_patch")
        self.assertEqual(self.current(3)["tool"], "Bash")
        self.record("PostToolUse", 4, call="a")
        self.assertNotIn("tool", self.current(5))
        self.assertEqual(self.current(5)["elapsed_s"], 5)

    def test_permission_and_blocking_question(self):
        self.record("PreToolUse", 1)
        self.record("PermissionRequest", 2)
        self.assertEqual(self.current(2)["state"], "waiting")
        self.record("PostToolUse", 3)
        self.assertEqual(self.current(3)["state"], "working")
        self.record("PreToolUse", 4, call="q", tool="request_user_input")
        self.assertEqual(self.current(4)["state"], "waiting")
        self.record("PostToolUse", 5, call="q", tool="request_user_input")
        self.assertEqual(self.current(5)["pending"], 0)

    def test_stop_interrupt_fade_and_late_events_do_not_reopen_turn(self):
        for terminal, state in (("Stop", "done"), ("Interrupt", "stopped")):
            self.setUp()
            self.record("UserPromptSubmit", 1)
            self.record(terminal, 10)
            self.record("PostToolUse", 11)
            self.record("UserPromptSubmit", 12)
            self.assertEqual(self.current(13)["state"], state)
            self.assertEqual(self.current(13)["elapsed_s"], 9)
            self.assertEqual(self.tracker.agent_status(14)["sessions"], [])
            self.record("UserPromptSubmit", 15, turn="next")
            self.record("PreToolUse", 16, turn="t")
            self.assertEqual(self.current(17)["elapsed_s"], 2)

    def test_session_end_and_out_of_order_pre_permission(self):
        self.record("PostToolUse", 5)
        self.tracker.record_status(event("PreToolUse", stamp=1000), 6)
        self.tracker.record_status(event("PermissionRequest", stamp=2000), 7)
        self.assertNotIn("tool", self.current(7))
        self.assertEqual(self.current(7)["state"], "working")
        self.record("SessionEnd", 8)
        self.tracker.record_status(event("PreToolUse", stamp=3000), 9)
        self.assertEqual(self.tracker.agent_status(9)["sessions"], [])

    def test_late_tool_after_session_end_needs_a_new_turn_and_cannot_reopen_old_one(self):
        self.record("PreToolUse", 1)
        self.record("SessionEnd", 2)
        self.record("PostToolUse", 3)
        self.assertFalse(self.tracker.sessions)
        self.record("SessionStart", 4, source="resume")
        self.record("PreToolUse", 5)  # old turn, even with a later handler timestamp
        self.assertEqual(self.tracker.agent_status(5)["sessions"], [])
        self.record("UserPromptSubmit", 6, turn="next")
        self.assertEqual(self.current(7)["elapsed_s"], 1)

    def test_resume_compact_and_duplicate_prompt_preserve_timer(self):
        self.record("UserPromptSubmit", 1)
        self.record("PreToolUse", 2)
        self.record("SessionStart", 3, source="resume")
        self.record("SessionStart", 4, source="compact")
        self.record("UserPromptSubmit", 5)
        self.assertEqual(self.current(9)["elapsed_s"], 8)
        self.assertEqual(self.current(9)["tool"], "Bash")

    def test_session_start_order_does_not_hide_first_or_resumed_turn(self):
        self.tracker.record_status(event("SessionStart", stamp=9000), 1)
        self.tracker.record_status(event("UserPromptSubmit", stamp=1000), 2)
        self.assertEqual(self.current(4)["elapsed_s"], 2)
        self.tracker.record_status(event("SessionStart", stamp=10000, source="resume"), 4)
        self.tracker.record_status(event("UserPromptSubmit", stamp=3000, turn="next"), 5)
        self.assertEqual(self.current(6)["elapsed_s"], 1)
        self.tracker.record_status(event("PreToolUse", stamp=4000, turn="t"), 7)
        self.assertNotIn("tool", self.current(7))

    def test_silence_is_not_completion_and_storage_is_bounded(self):
        for i in range(MAX_TRACKED_SESSIONS * 2): self.record("PreToolUse", i+1, session=str(i))
        self.assertEqual(len(self.tracker.sessions), MAX_TRACKED_SESSIONS)
        self.assertTrue(all(s["state"] == "working" for s in self.tracker.agent_status(3600)["sessions"]))
        self.tracker.expire(30000)
        self.assertFalse(self.tracker.sessions)

    def test_hidden_waiting_session_and_timer_survive_more_than_four_tasks(self):
        claude = ClaudeBroker()
        self.record("PermissionRequest", 1, session="waiting")
        for i in range(6): self.record("PreToolUse", i+2, session=str(i))
        packet = merged_agent_status(claude, self.tracker, 8)
        self.assertEqual(len(packet["sessions"]), 4)
        wait = next(s for s in packet["sessions"] if s["state"] == "waiting")
        self.assertEqual(wait["elapsed_s"], 7)
        self.record("PreToolUse", 10, session="0", call="next")
        packet = merged_agent_status(claude, self.tracker, 11)
        active = next(s for s in packet["sessions"] if s["id"] == codex_hook.digest("0", 8))
        self.assertEqual(active["elapsed_s"], 9)

    def test_reject_unexpected_fields_and_types(self):
        for change in ({"prompt": "secret"}, {"v": True}, {"stamp_ns": True}, {"session": "BAD"}, {"turn": "raw"}, {"project": ""}):
            with self.assertRaises(ValueError): self.tracker.record_status(dict(event(), **change), 1)

    def test_provider_fairness_colliding_id_unicode_and_wire_size(self):
        claude = ClaudeBroker()
        for i in range(4):
            claude.record_status({"v":1,"event":"UserPromptSubmit","session":f"{i:08x}","project":"项"*80}, i)
        self.record("PreToolUse", 5, cwd="/" + "项"*80, tool="界"*80)
        original = next(iter(self.tracker.sessions))
        renamed = self.tracker.sessions.pop(original)
        renamed.key = "00000000"
        self.tracker.sessions[renamed.key] = renamed
        packet = merged_agent_status(claude, self.tracker, 6)
        validate_agent_status(packet)
        self.assertEqual(len(packet["sessions"]), 4)
        self.assertEqual({s["source"] for s in packet["sessions"]}, {"claude", "codex"})
        self.assertLess(len(json.dumps(packet, ensure_ascii=False).encode()), 2048)
        self.assertTrue(all(len(s["project"].encode()) <= 32 for s in packet["sessions"]))

    def test_selection_uses_latest_activity_even_for_same_tool(self):
        claude = ClaudeBroker()
        claude.record_status({"v":1,"event":"UserPromptSubmit","session":"00000001","project":"eps32"}, 10)
        self.record("PreToolUse", 1, session="fresh", call="first")
        for i in range(3): self.record("PreToolUse", i+4, session=str(i))
        self.record("PreToolUse", 20, session="fresh", call="second")
        packet = merged_agent_status(claude, self.tracker, 21)
        fresh = next(s for s in packet["sessions"] if s["source"] == "codex" and s["id"] == codex_hook.digest("fresh", 8))
        self.assertEqual(fresh["age_s"], 20)  # unchanged state, newly active task
        self.assertEqual(len(packet["sessions"]), 4)
        self.assertEqual({s["source"] for s in packet["sessions"]}, {"claude", "codex"})

    def test_merged_terminal_window_is_four_seconds_without_changing_legacy(self):
        claude = ClaudeBroker()
        for name, now in (("UserPromptSubmit", 0), ("Stop", 1)):
            claude.record_status({"v":1,"event":name,"session":"00000001","project":"eps32"}, now)
        self.record("UserPromptSubmit", 2)
        self.assertEqual(len(merged_agent_status(claude, self.tracker, 4)["sessions"]), 2)
        self.assertEqual(len(merged_agent_status(claude, self.tracker, 5)["sessions"]), 1)
        self.assertEqual(len(claude.agent_status(5)["sessions"]), 1)


class Device:
    remote_address = ("127.0.0.1", 9000)
    def __init__(self): self.messages = []
    async def send(self, message): self.messages.append(json.loads(message))


class BridgeTests(unittest.IsolatedAsyncioTestCase):
    async def test_old_new_and_unpaired_negotiation(self):
        with tempfile.TemporaryDirectory() as tmp:
            bridge = Bridge(WallpaperLibrary(Path(tmp)), pairing_key=b"x"*16)
            bridge.claude.record_status({"v":1,"event":"UserPromptSubmit","session":"00000001","project":"eps32"}, 1)
            await bridge.codex_api(event(stamp=1000))
            old, new, unpaired = Device(), Device(), Device()
            for d, caps in ((old,{"claude_companion"}), (new,{"agent_status_v2"}), (unpaired,{"agent_status_v2"})):
                bridge.ready_clients.add(d)
                bridge.clients.add(d)
                bridge.send_locks[d] = asyncio.Lock()
                bridge.client_capabilities[d] = frozenset(caps)
            bridge.authenticated_clients.update({old,new})
            await bridge._send_agent_status(2)
            self.assertTrue(all("source" not in s for s in old.messages[0]["sessions"]))
            self.assertEqual({s["source"] for s in new.messages[0]["sessions"]}, {"claude","codex"})
            self.assertFalse(unpaired.messages)


class HttpTests(unittest.TestCase):
    def test_codex_endpoint_checks_origin_content_and_payload(self):
        tracker = CodexTracker()
        def callback(payload):
            f = Future()
            try:
                tracker.record_status(payload, 1)
                f.set_result({"ok":True})
            except Exception as e: f.set_exception(e)
            return f
        with tempfile.TemporaryDirectory() as tmp:
            service = WallpaperEditorService(WallpaperLibrary(Path(tmp)), Path(tmp), 0,
                                              lambda x: None, lambda: None, lambda x: None, codex=callback)
            service.start()
            try:
                url = f"http://127.0.0.1:{service.port}/api/codex/status"
                def post(payload, headers):
                    return urlopen(Request(url, data=json.dumps(payload).encode(), headers=headers),timeout=2)
                with post(event(), {"Content-Type":"application/json"}) as r: self.assertEqual(r.status,200)
                for payload, headers, code in (
                    (event(),{"Content-Type":"text/plain"},403),
                    (event(),{"Content-Type":"application/json","Origin":"http://evil.example"},403),
                    (dict(event(),prompt="secret"),{"Content-Type":"application/json"},400)):
                    with self.assertRaises(HTTPError) as e: post(payload,headers)
                    self.assertEqual(e.exception.code,code)
            finally: service.stop()


if __name__ == "__main__": unittest.main()
