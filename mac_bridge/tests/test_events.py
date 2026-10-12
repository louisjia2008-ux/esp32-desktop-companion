from __future__ import annotations

import asyncio
from concurrent.futures import Future
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

from bridge import Bridge, action_required_message, notification_message, parse_device_message, ProtocolError
from events import EventError, EventLedger, MAX_RECENT, validate_event
from wallpaper_library import WallpaperLibrary
from wallpaper_http import WallpaperEditorService


class EventProtocolTests(unittest.TestCase):
    def test_event_levels_defaults_and_byte_limits(self):
        for level in ("info", "success", "warning", "error"):
            packet = notification_message(level)
            self.assertEqual(validate_event(packet), packet)
        valid = action_required_message()
        for count in (1, 2, 3):
            candidate = dict(valid, actions=valid["actions"][:count])
            self.assertEqual(validate_event(candidate), candidate)
        invalids = [dict(valid, v=True), dict(valid, v=2), dict(valid, command="id"),
                    dict(valid, actions=[]), dict(valid, actions=valid["actions"] * 2),
                    dict(valid, actions=[valid["actions"][0]] * 2),
                    dict(valid, source="界" * 11), dict(valid, message="x" * 129),
                    dict(valid, request_id="a\u0000b"), dict(valid, title="\ud800")]
        for packet in invalids:
            with self.subTest(packet=packet), self.assertRaises(EventError):
                validate_event(packet)
        for level in ("unknown", None, True, ["info"], {"info": 1}):
            packet = dict(notification_message(), level=level)
            with self.subTest(level=level), self.assertRaises(EventError):
                validate_event(packet)
        for duration in (-1, True, 3.5, 999, 600001):
            with self.assertRaises(EventError):
                validate_event(dict(notification_message(), auto_dismiss_ms=duration))

    def test_device_action_has_no_extension_or_command_fields(self):
        packet = {"v": 1, "type": "action", "request_id": "demo-001", "action_id": "refactor"}
        self.assertEqual(parse_device_message(json.dumps(packet)), packet)
        for invalid in (dict(packet, command="id"), dict(packet, action_id="../a"),
                        dict(packet, request_id="a\u0000b"), dict(packet, action_id="\ud800")):
            with self.assertRaises(ProtocolError):
                parse_device_message(json.dumps(invalid))

    def test_ledger_retry_conflict_and_restart_are_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "events.json"
            ledger = EventLedger(path)
            packet = action_required_message()
            self.assertTrue(ledger.register(packet))
            self.assertFalse(ledger.register(packet))
            self.assertEqual(ledger.choose("unknown", "refactor"), (False, False))
            self.assertEqual(ledger.choose("demo-001", "unknown"), (False, False))
            self.assertEqual(ledger.choose("demo-001", "refactor"), (True, False))
            self.assertEqual(ledger.choose("demo-001", "refactor"), (True, True))
            self.assertEqual(ledger.choose("demo-001", "minimal"), (False, True))
            self.assertEqual(ledger.action_processed, 1)
            restarted = EventLedger(path)
            self.assertEqual(restarted.choose("demo-001", "refactor"), (True, True))
            self.assertEqual(restarted.action_processed, 0)
            self.assertEqual(restarted.next_demo_id(), "demo-002")
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(EventError):
                restarted.register(dict(packet, title="Different"))

    def test_pending_records_are_never_evicted_for_new_requests(self):
        ledger = EventLedger()
        for i in range(MAX_RECENT): ledger.register(action_required_message(f"request-{i}"))
        with self.assertRaises(EventError): ledger.register(action_required_message("full"))
        self.assertEqual(len(ledger.records), MAX_RECENT)
        ledger.choose("request-3", "minimal")
        ledger.register(action_required_message("new"))
        self.assertNotIn("request-3", ledger.records)
        self.assertIn("request-0", ledger.records)

    def test_ledger_write_failure_does_not_accept_selection(self):
        ledger = EventLedger()
        ledger.register(action_required_message())
        with mock.patch.object(ledger, "_save", side_effect=OSError("unavailable")):
            with self.assertRaises(OSError): ledger.choose("demo-001", "refactor")
        self.assertIsNone(ledger.records["demo-001"]["selected"])
        self.assertEqual(ledger.action_processed, 0)

    def test_notification_dedup_is_bounded_and_copies_payloads(self):
        ledger = EventLedger()
        packet = notification_message(event_id="same")
        self.assertTrue(ledger.register(packet))
        self.assertFalse(ledger.register(packet))
        with self.assertRaises(EventError): ledger.register(dict(packet, title="Different"))
        for i in range(1000): ledger.register(notification_message(event_id=f"event-{i}"))
        self.assertEqual(len(ledger.notifications), MAX_RECENT)

    def test_loopback_publisher_rejects_cross_origin_and_malformed(self):
        with tempfile.TemporaryDirectory() as temporary:
            def completed(value):
                future = Future(); future.set_result(value); return future
            def publish(packet):
                future = Future()
                try: future.set_result({"delivered_clients": 1, "packet": validate_event(packet)})
                except EventError as error: future.set_exception(error)
                return future
            try:
                editor = WallpaperEditorService(WallpaperLibrary(Path(temporary)),
                    Path(__file__).resolve().parents[1], 0, lambda _: completed({}),
                    lambda: completed({}), lambda _: completed({}), event=publish)
            except PermissionError:
                self.skipTest("loopback bind unavailable in sandbox")
            editor.start()
            try:
                uri = f"http://127.0.0.1:{editor.port}/api/events"
                raw = json.dumps(notification_message()).encode()
                req = Request(uri, data=raw, headers={"Content-Type": "application/json"})
                with urlopen(req, timeout=3) as response:
                    self.assertEqual(json.load(response)["delivered_clients"], 1)
                for body, headers, expected in ((raw, {"Content-Type": "application/json", "Origin": "https://untrusted.invalid"}, 403),
                    (b"{", {"Content-Type": "application/json"}, 400),
                    (b'{"v":1,"type":"shell","command":"id"}', {"Content-Type": "application/json"}, 400)):
                    with self.assertRaises(HTTPError) as context:
                        urlopen(Request(uri, data=body, headers=headers), timeout=3)
                    self.assertEqual(context.exception.code, expected)
            finally: editor.stop()


class BridgeActionTests(unittest.IsolatedAsyncioTestCase):
    async def test_real_socket_ack_retry_and_bridge_restart(self):
        # This simulator is confined to an ephemeral loopback server; it never
        # participates in the physical device's LAN requests or acceptance.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ledger_path = root / "ledger.json"
            for restarted in (False, True):
                bridge = Bridge(WallpaperLibrary(root / "wallpapers"), event_ledger=EventLedger(ledger_path))
                async with serve(bridge.handle_client, "127.0.0.1", 0, subprotocols=["arduino"]) as server:
                    port = server.sockets[0].getsockname()[1]
                    async with connect(f"ws://127.0.0.1:{port}", subprotocols=["arduino"]) as connection:
                        await connection.send(json.dumps({"v":1, "type":"hello",
                            "device":"esp32-companion-simulator", "firmware":"test",
                            "width":480, "height":320, "capabilities":["interactive_events"]}))
                        for _ in range(100):
                            if bridge.ready_clients: break
                            await asyncio.sleep(0.01)
                        self.assertEqual(len(bridge.ready_clients), 1)
                        if not restarted:
                            await bridge.publish_event(action_required_message("socket-001"))
                            offered = json.loads(await asyncio.wait_for(connection.recv(), timeout=2))
                            self.assertEqual(offered["request_id"], "socket-001")
                        for action_id, accepted in (("refactor", True), ("refactor", True), ("minimal", False), ("unknown", False)):
                            await connection.send(json.dumps({"v":1, "type":"action", "request_id":"socket-001", "action_id":action_id}))
                            ack = json.loads(await asyncio.wait_for(connection.recv(), timeout=2))
                            self.assertEqual(ack, {"v":1,"type":"action_ack","request_id":"socket-001","action_id":action_id,"accepted":accepted})
                        self.assertEqual(bridge.event_ledger.action_processed, 0 if restarted else 1)
                        self.assertEqual(bridge.event_ledger.duplicate_action_count, 0)

    async def test_ack_goes_only_to_sender_and_processing_is_once(self):
        class Client:
            def __init__(self): self.sent = []
            async def send(self, raw): self.sent.append(json.loads(raw))
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Bridge(WallpaperLibrary(Path(temporary)))
            sender, other = Client(), Client()
            for client in (sender, other):
                bridge.clients.add(client); bridge.ready_clients.add(client)
                bridge.send_locks[client] = asyncio.Lock()
                bridge.client_capabilities[client] = frozenset({"interactive_events"})
            await bridge.publish_event(action_required_message())
            sender.sent.clear(); other.sent.clear()
            action = {"v":1, "type":"action", "request_id":"demo-001", "action_id":"refactor"}
            await bridge.handle_device_message(sender, action)
            await bridge.handle_device_message(sender, action)
            self.assertEqual(len(sender.sent), 2)
            self.assertTrue(all(a["type"] == "action_ack" and a["accepted"] for a in sender.sent))
            self.assertEqual(other.sent, [])
            self.assertEqual(bridge.event_ledger.action_processed, 1)
            await bridge.handle_device_message(sender, dict(action, action_id="minimal"))
            self.assertFalse(sender.sent[-1]["accepted"])

    async def test_old_firmware_does_not_receive_new_events(self):
        class Client:
            async def send(self, _): raise AssertionError("event sent to old firmware")
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Bridge(WallpaperLibrary(Path(temporary)))
            client = Client()
            bridge.ready_clients.add(client)
            bridge.client_capabilities[client] = frozenset({"host_status"})
            result = await bridge.publish_event(action_required_message())
            self.assertEqual(result["delivered_clients"], 0)
            self.assertEqual(len(bridge.event_ledger.records), 0)
