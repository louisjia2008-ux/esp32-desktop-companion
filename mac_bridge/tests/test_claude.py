from __future__ import annotations

import asyncio
from concurrent.futures import Future
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import claude_hook
from bridge import Bridge, claude_hooks_settings, load_or_create_pairing_key, pairing_mac
from claude_sessions import (
    LEASE_SECONDS,
    MAX_SESSIONS,
    ClaudeBroker,
    ClaudeError,
    utf8_prefix,
    validate_agent_status,
)
from events import EventError, validate_event
from wallpaper_http import WallpaperEditorService
from wallpaper_library import WallpaperLibrary

SESSION = "a1b2c3d4"
FP = "0123456789abcdef"


def status(event, *, session=SESSION, tool=None, fingerprint=None, project="eps32"):
    payload = {"v": 1, "event": event, "session": session, "project": project}
    if tool is not None:
        payload["tool"] = tool
    if fingerprint is not None:
        payload["fingerprint"] = fingerprint
    return payload


def permission(summary="$ rm -rf build", *, fingerprint=FP, session=SESSION):
    return {"v": 1, "session": session, "project": "eps32", "kind": "permission",
            "tool": "Bash", "summary": summary, "fingerprint": fingerprint}


def question(*questions, fingerprint=FP):
    return {"v": 1, "session": SESSION, "project": "eps32", "kind": "question",
            "fingerprint": fingerprint, "questions": list(questions)}


def q(text, options, *, header="", multi=False):
    return {"header": header, "question": text, "options": options, "multi": multi}


class BrokerTests(unittest.TestCase):
    def test_permission_card_choice_and_idempotent_replay(self):
        broker = ClaudeBroker()
        request = broker.open_request(permission(), 10.0)
        card = request.cards[0]
        self.assertEqual(validate_event(card), card)
        self.assertEqual(card["request_id"], f"{request.request_id}-0")
        self.assertEqual([a["id"] for a in card["actions"]], ["allow", "deny"])
        self.assertEqual(card["actions"][0]["hold_ms"], 800)
        self.assertEqual(card["message"], "$ rm -rf build")
        self.assertEqual(broker.poll(request.request_id, 10.5), {"status": "pending", "question": 0})
        self.assertEqual(broker.choose(card["request_id"], "allow", 11.0), (True, None))
        self.assertEqual(broker.choose(card["request_id"], "allow", 11.1), (True, None))
        self.assertEqual(broker.choose(card["request_id"], "deny", 11.2), (False, None))
        self.assertEqual(broker.poll(request.request_id, 11.3),
                         {"status": "answered", "behavior": "allow"})

    def test_unknown_or_foreign_choices_are_rejected(self):
        broker = ClaudeBroker()
        request = broker.open_request(permission(), 0.0)
        for card_id, action in ((request.cards[0]["request_id"], "o0"),
                                ("cc-000000000000-0", "allow"),
                                (f"{request.request_id}-3", "allow"),
                                ("demo-001", "allow")):
            self.assertEqual(broker.choose(card_id, action, 1.0), (False, None))
        self.assertEqual(broker.poll(request.request_id, 1.0)["status"], "pending")

    def test_multiple_questions_are_answered_card_by_card(self):
        broker = ClaudeBroker()
        request = broker.open_request(question(
            q("用哪种字体?", ["u8g2", "位图", "不显示中文"], header="中文"),
            q("Ship now?", ["Yes", "No", "Later", "Ask me"])), 0.0)
        first, second = request.cards
        self.assertEqual(first["title"], "中文 1/2")
        self.assertEqual(len(second["actions"]), 4)
        self.assertEqual(broker.choose(second["request_id"], "o0", 1.0), (False, None))
        accepted, next_card = broker.choose(first["request_id"], "o2", 1.0)
        self.assertTrue(accepted)
        self.assertEqual(next_card, second)
        self.assertEqual(broker.poll(request.request_id, 1.1)["status"], "pending")
        self.assertEqual(broker.choose(second["request_id"], "o1", 2.0), (True, None))
        self.assertEqual(broker.poll(request.request_id, 2.1), {"status": "answered", "answers": [2, 1]})

    def test_multi_select_questions_stay_in_the_terminal(self):
        broker = ClaudeBroker()
        self.assertIsNone(broker.open_request(question(q("Which?", ["a", "b"], multi=True)), 0.0))
        self.assertEqual(broker.counters["unsupported"], 1)

    def test_terminal_answer_cancels_by_fingerprint_or_turn_end(self):
        broker = ClaudeBroker()
        request = broker.open_request(permission(), 0.0)
        # PreToolUse races with PermissionRequest and must never cancel.
        self.assertEqual(broker.record_status(status("PreToolUse", tool="Bash", fingerprint=FP), 0.1), [])
        self.assertEqual(broker.record_status(
            status("PostToolUse", tool="Read", fingerprint="f" * 16), 0.2), [])
        self.assertEqual(broker.record_status(
            status("PostToolUse", tool="Bash", fingerprint=FP), 0.3), [request.cards[0]["request_id"]])
        self.assertEqual(broker.poll(request.request_id, 0.4), {"status": "cancelled"})
        second = broker.open_request(permission(fingerprint="e" * 16), 1.0)
        self.assertEqual(broker.record_status(status("Stop"), 2.0), [second.cards[0]["request_id"]])
        self.assertEqual(broker.choose(second.cards[0]["request_id"], "allow", 2.1), (False, None))

    def test_other_sessions_are_not_cancelled(self):
        broker = ClaudeBroker()
        request = broker.open_request(permission(session="bbbbbbbb"), 0.0)
        self.assertEqual(broker.record_status(status("Stop"), 1.0), [])
        self.assertEqual(broker.poll(request.request_id, 1.0)["status"], "pending")

    def test_lease_expires_without_polls(self):
        broker = ClaudeBroker()
        request = broker.open_request(permission(), 0.0)
        for now in (1.0, 2.0, 3.0, 4.0):
            broker.poll(request.request_id, now)
            self.assertEqual(broker.expire(now), [])
        self.assertEqual(broker.expire(4.0 + LEASE_SECONDS + 0.1), [request.cards[0]["request_id"]])
        self.assertEqual(broker.poll(request.request_id, 9.0), {"status": "expired"})
        self.assertEqual(broker.sessions[SESSION].state, "working")

    def test_agent_status_states_ordering_and_bounds(self):
        broker = ClaudeBroker()
        broker.record_status(status("UserPromptSubmit", session="00000001", project="alpha"), 0.0)
        broker.record_status(status("PreToolUse", session="00000001", tool="Edit", project="alpha"), 1.0)
        broker.record_status(status("UserPromptSubmit", session="00000002", project="beta"), 2.0)
        broker.open_request(permission(session="00000002"), 3.0)
        broker.record_status(status("UserPromptSubmit", session="00000003", project="gamma"), 0.0)
        broker.record_status(status("Stop", session="00000003", project="gamma"), 30.0)
        message = validate_agent_status(broker.agent_status(35.0))
        states = [(s["project"], s["state"]) for s in message["sessions"]]
        self.assertEqual(states, [("eps32", "waiting"), ("alpha", "working"), ("gamma", "done")])
        self.assertEqual(message["sessions"][1]["tool"], "Edit")
        self.assertEqual(message["sessions"][1]["elapsed_s"], 35)
        self.assertEqual(message["sessions"][2]["elapsed_s"], 30)
        self.assertEqual(message["sessions"][0]["pending"], 1)
        # A finished turn disappears from the device after the done window.
        self.assertNotIn("gamma", [s["project"] for s in broker.agent_status(41.0)["sessions"]])

    def test_sessions_are_bounded_and_packets_fit_the_wire(self):
        broker = ClaudeBroker()
        for index in range(MAX_SESSIONS + 3):
            broker.record_status(status("PreToolUse", session=f"{index:08x}", tool="界" * 40,
                                        project="项" * 80), float(index))
        self.assertEqual(len(broker.sessions), MAX_SESSIONS)
        message = broker.agent_status(10.0)
        validate_agent_status(message)
        self.assertLess(len(json.dumps(message, ensure_ascii=False).encode()), 2048)
        self.assertTrue(all(len(s["project"].encode()) <= 32 for s in message["sessions"]))

    def test_malformed_payloads_are_rejected(self):
        broker = ClaudeBroker()
        bad = [dict(status("Stop"), v=2), dict(status("Stop"), session="XYZ"),
               dict(status("Stop"), prompt="secret"), dict(status("Stop"), event="Shell"),
               dict(status("Stop"), fingerprint="xyz"), "Stop", None]
        for payload in bad:
            with self.subTest(payload=payload), self.assertRaises(ClaudeError):
                broker.record_status(payload, 0.0)
        for payload in (dict(permission(), kind="shell"), dict(permission(), command="id"),
                        question(q("x", ["only one"])), question(*[q("x", ["a", "b"])] * 5)):
            with self.subTest(payload=payload), self.assertRaises(ClaudeError):
                broker.open_request(payload, 0.0)
        with self.assertRaises(ClaudeError):
            broker.poll("../etc", 0.0)

    def test_utf8_prefix_never_splits_characters_or_keeps_controls(self):
        self.assertEqual(utf8_prefix("a\nb\tc", 16), "a b c")
        cut = utf8_prefix("中" * 50, 48)
        self.assertLessEqual(len(cut.encode()), 48)
        self.assertTrue(cut.endswith("…"))
        cut.encode("utf-8")


class HookTests(unittest.TestCase):
    def test_status_payload_forwards_only_whitelisted_fields(self):
        data = {"hook_event_name": "UserPromptSubmit", "session_id": "abc",
                "cwd": "/Users/me/secret/project-x", "prompt": "my private prompt",
                "transcript_path": "/Users/me/.claude/x.jsonl"}
        payload = claude_hook.status_payload(data)
        self.assertEqual(set(payload), {"v", "event", "session", "project"})
        self.assertEqual(payload["project"], "project-x")
        self.assertNotIn("private", json.dumps(payload))
        self.assertIsNone(claude_hook.status_payload({"hook_event_name": "PermissionRequest"}))
        tool = claude_hook.status_payload({"hook_event_name": "PostToolUse", "session_id": "abc",
                                           "cwd": "/x/y", "tool_name": "Bash",
                                           "tool_input": {"command": "ls"}, "tool_response": "a.txt"})
        self.assertEqual(set(tool), {"v", "event", "session", "project", "tool", "fingerprint"})

    def test_fingerprint_matches_before_and_after_answers(self):
        before = {"questions": [{"question": "Q", "options": []}]}
        after = dict(before, answers={"Q": "A"})
        self.assertEqual(claude_hook.fingerprint(before), claude_hook.fingerprint(after))
        self.assertNotEqual(claude_hook.fingerprint({"command": "ls"}),
                            claude_hook.fingerprint({"command": "ls -a"}))

    def test_permission_summaries_avoid_full_paths_and_content(self):
        summary = claude_hook.permission_summary
        self.assertEqual(summary("Bash", {"command": "make test\nrm -rf /"}), "$ make test")
        self.assertEqual(summary("Edit", {"file_path": "/Users/me/secret/main.cpp",
                                          "new_string": "password=1"}), "main.cpp")
        self.assertEqual(summary("WebFetch", {"url": "https://example.com/a?token=x"}), "example.com")
        self.assertEqual(summary("mcp__srv__tool", {"anything": "x"}), "")

    def test_request_payload_is_accepted_by_the_broker(self):
        data = {"hook_event_name": "PermissionRequest", "session_id": "s", "cwd": "/a/eps32",
                "tool_name": "AskUserQuestion",
                "tool_input": {"questions": [{"question": "Color?", "header": "Color",
                                              "options": [{"label": "Red", "description": "r"},
                                                          {"label": "Blue", "description": "b"}],
                                              "multiSelect": False}]}}
        payload = claude_hook.request_payload(data)
        request = ClaudeBroker().open_request(payload, 0.0)
        self.assertEqual([a["label"] for a in request.cards[0]["actions"]], ["Red", "Blue"])
        output = claude_hook.decision_output(data, {"status": "answered", "answers": [1]})
        decision = output["hookSpecificOutput"]["decision"]
        self.assertEqual(decision["behavior"], "allow")
        self.assertEqual(decision["updatedInput"]["answers"], {"Color?": "Blue"})
        self.assertEqual(decision["updatedInput"]["questions"], data["tool_input"]["questions"])
        self.assertIsNone(claude_hook.decision_output(data, {"status": "answered", "answers": [5]}))
        self.assertIsNone(claude_hook.decision_output(data, {"status": "cancelled"}))

    def test_permission_decisions(self):
        data = {"tool_name": "Bash"}
        allow = claude_hook.decision_output(data, {"status": "answered", "behavior": "allow"})
        deny = claude_hook.decision_output(data, {"status": "answered", "behavior": "deny"})
        self.assertEqual(allow["hookSpecificOutput"]["decision"], {"behavior": "allow"})
        self.assertEqual(deny["hookSpecificOutput"]["decision"]["behavior"], "deny")
        self.assertIsNone(claude_hook.decision_output(data, {"status": "answered", "behavior": "x"}))

    def test_hook_is_silent_when_bridge_is_unreachable(self):
        stdin = io.StringIO(json.dumps({"hook_event_name": "PermissionRequest", "session_id": "s",
                                        "cwd": "/x", "tool_name": "Bash",
                                        "tool_input": {"command": "ls"}}))
        stdout = io.StringIO()
        with mock.patch.object(claude_hook, "BRIDGE_URL", "http://127.0.0.1:9"), \
                mock.patch("sys.stdin", stdin), mock.patch("sys.stdout", stdout):
            self.assertEqual(claude_hook.main(), 0)
        self.assertEqual(stdout.getvalue(), "")

    def test_settings_fragment_registers_every_event(self):
        settings = claude_hooks_settings(Path("/x/claude_hook.py"))
        self.assertEqual(set(settings["hooks"]), {
            "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse",
            "PermissionRequest", "Notification", "Stop", "SessionEnd"})
        permission_hook = settings["hooks"]["PermissionRequest"][0]
        self.assertEqual(permission_hook["matcher"], "*")
        self.assertGreater(permission_hook["hooks"][0]["timeout"], claude_hook.DECISION_TIMEOUT)


class FakeDevice:
    def __init__(self):
        self.sent = []

    async def send(self, raw):
        self.sent.append(json.loads(raw))

    def of_type(self, kind):
        return [m for m in self.sent if m.get("type") == kind]


HELLO = {"v": 1, "type": "hello", "device": "sim", "firmware": "t", "width": 480, "height": 320}


class BridgeClaudeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.now = [100.0]
        self.key = bytes(range(16))
        self.bridge = Bridge(WallpaperLibrary(Path(self.directory.name)),
                             monotonic_clock=lambda: self.now[0], pairing_key=self.key)

    async def asyncTearDown(self):
        self.directory.cleanup()

    async def connect(self, capabilities, *, mac=None):
        device = FakeDevice()
        self.bridge.clients.add(device)
        self.bridge.send_locks[device] = asyncio.Lock()
        await self.bridge.handle_device_message(device, dict(HELLO, capabilities=capabilities))
        challenge = device.of_type("auth_challenge")
        if challenge and mac is not False:
            nonce = challenge[0]["nonce"]
            await self.bridge.handle_device_message(device, {
                "v": 1, "type": "auth_response", "mac": mac or pairing_mac(self.key, nonce)})
        return device

    async def test_paired_device_answers_a_permission_request(self):
        device = await self.connect(["interactive_events", "events_v2", "claude_companion"])
        self.assertEqual(len(device.of_type("agent_status")), 1)
        opened = await self.bridge.claude_api("open", permission())
        self.assertEqual(opened["status"], "pending")
        card = device.of_type("action_required")[0]
        await self.bridge.handle_device_message(device, {
            "v": 1, "type": "action", "request_id": card["request_id"], "action_id": "deny"})
        self.assertEqual(device.of_type("action_ack")[-1]["accepted"], True)
        self.assertEqual(await self.bridge.claude_api("poll", opened["request_id"]),
                         {"status": "answered", "behavior": "deny"})
        # Claude requests never enter the persistent generic ledger.
        self.assertEqual(len(self.bridge.event_ledger.records), 0)

    async def test_wrong_key_or_missing_capability_gets_nothing(self):
        wrong = await self.connect(["interactive_events", "claude_companion"], mac="0" * 64)
        legacy = await self.connect(["interactive_events"])
        unanswered = await self.connect(["interactive_events", "claude_companion"], mac=False)
        self.assertEqual(await self.bridge.claude_api("open", permission()), {"status": "no_device"})
        for device in (wrong, legacy, unanswered):
            self.assertEqual(device.of_type("action_required"), [])
            self.assertEqual(device.of_type("agent_status"), [])
        self.assertEqual(legacy.of_type("auth_challenge"), [])

    async def test_unpaired_device_cannot_answer_a_claude_card(self):
        paired = await self.connect(["interactive_events", "events_v2", "claude_companion"])
        intruder = await self.connect(["interactive_events", "claude_companion"], mac=False)
        opened = await self.bridge.claude_api("open", permission())
        card = paired.of_type("action_required")[0]
        await self.bridge.handle_device_message(intruder, {
            "v": 1, "type": "action", "request_id": card["request_id"], "action_id": "allow"})
        self.assertFalse(intruder.of_type("action_ack")[-1]["accepted"])
        self.assertEqual((await self.bridge.claude_api("poll", opened["request_id"]))["status"], "pending")

    async def test_terminal_answer_dismisses_the_card(self):
        device = await self.connect(["interactive_events", "events_v2", "claude_companion"])
        opened = await self.bridge.claude_api("open", permission())
        card_id = device.of_type("action_required")[0]["request_id"]
        await self.bridge.claude_api("status", status("PostToolUse", tool="Bash", fingerprint=FP))
        self.assertEqual(device.of_type("dismiss"), [{"v": 1, "type": "dismiss", "request_id": card_id}])
        self.assertEqual(await self.bridge.claude_api("poll", opened["request_id"]), {"status": "cancelled"})

    async def test_tick_expires_leases_and_pushes_status(self):
        device = await self.connect(["interactive_events", "events_v2", "claude_companion"])
        await self.bridge.claude_api("open", permission())
        self.now[0] += LEASE_SECONDS + 1
        await self.bridge._claude_tick()
        self.assertEqual(len(device.of_type("dismiss")), 1)
        self.assertGreaterEqual(len(device.of_type("agent_status")), 2)

    async def test_extended_generic_events_skip_old_firmware(self):
        old = await self.connect(["interactive_events"])
        new = await self.connect(["interactive_events", "events_v2"])
        packet = {"v": 1, "type": "action_required", "request_id": "four-001", "source": "demo",
                  "title": "Pick", "message": "Four options",
                  "actions": [{"id": f"a{i}", "label": f"A{i}"} for i in range(4)]}
        result = await self.bridge.publish_event(packet)
        self.assertEqual(result["delivered_clients"], 1)
        self.assertEqual(old.of_type("action_required"), [])
        self.assertEqual(len(new.of_type("action_required")), 1)
        with self.assertRaises(EventError):
            await self.bridge.publish_event(dict(packet, actions=packet["actions"] * 2))


class PairingKeyTests(unittest.TestCase):
    def test_key_is_created_once_private_and_validated(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime" / "pairing.key"
            key = load_or_create_pairing_key(path)
            self.assertEqual(len(key), 16)
            self.assertEqual(load_or_create_pairing_key(path), key)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            path.write_text("not-hex")
            with self.assertRaises(ValueError):
                load_or_create_pairing_key(path)


class HttpTests(unittest.TestCase):
    def test_hook_api_is_loopback_json_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            def completed(value):
                future = Future(); future.set_result(value); return future
            calls = []
            def claude(operation, payload):
                calls.append((operation, payload))
                future = Future()
                if payload == "bad":
                    future.set_exception(ClaudeError("bad"))
                else:
                    future.set_result({"status": "pending", "operation": operation})
                return future
            try:
                editor = WallpaperEditorService(WallpaperLibrary(Path(temporary)),
                    Path(__file__).resolve().parents[1], 0, lambda _: completed({}),
                    lambda: completed({}), lambda _: completed({}), claude=claude)
            except PermissionError:
                self.skipTest("loopback bind unavailable in sandbox")
            editor.start()
            try:
                base = f"http://127.0.0.1:{editor.port}"
                body = json.dumps(status("Stop")).encode()
                with urlopen(Request(base + "/api/claude/status", data=body,
                                     headers={"Content-Type": "application/json"}), timeout=3) as r:
                    self.assertEqual(json.load(r)["operation"], "status")
                with urlopen(base + "/api/claude/requests/cc-0123456789ab", timeout=3) as r:
                    self.assertEqual(json.load(r)["operation"], "poll")
                for url, data, headers, expected in (
                        ("/api/claude/status", body, {"Content-Type": "text/plain"}, 403),
                        ("/api/claude/status", body, {"Content-Type": "application/json",
                                                      "Origin": "https://evil.invalid"}, 403),
                        ("/api/claude/requests", body, {"Content-Type": "application/json",
                                                        "Sec-Fetch-Site": "cross-site"}, 403),
                        ("/api/claude/requests", b"{", {"Content-Type": "application/json"}, 400),
                        ("/api/claude/requests/bad", None, {}, 400),
                        ("/api/claude/requests/x", None, {"Host": "evil.invalid"}, 403)):
                    with self.subTest(url=url, headers=headers), self.assertRaises(HTTPError) as context:
                        urlopen(Request(base + url, data=data, headers=headers), timeout=3)
                    self.assertEqual(context.exception.code, expected)
            finally:
                editor.stop()


if __name__ == "__main__":
    unittest.main()
