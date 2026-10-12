from __future__ import annotations

import asyncio
from concurrent.futures import Future
import json
from pathlib import Path
import tempfile
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from bridge import Bridge
from display_settings import (
    DEFAULTS,
    DisplaySettingsError,
    DisplaySettingsStore,
    clock_message,
    validate,
)
from wallpaper_http import WallpaperEditorService
from wallpaper_library import WallpaperLibrary


class FakeDevice:
    def __init__(self):
        self.sent = []

    async def send(self, raw):
        self.sent.append(json.loads(raw))


HELLO = {"v": 1, "type": "hello", "device": "sim", "firmware": "t", "width": 480, "height": 320}


class DisplaySettingsTests(unittest.TestCase):
    def test_validation_matches_firmware_invariants(self):
        self.assertEqual(validate(dict(DEFAULTS)), DEFAULTS)
        bad = [dict(DEFAULTS, night_start_min=1440), dict(DEFAULTS, day_level=4),
               dict(DEFAULTS, night_wake_level=5), dict(DEFAULTS, idle_level=101),
               dict(DEFAULTS, idle_dim_s=3601), dict(DEFAULTS, day_level=True),
               dict(DEFAULTS, extra=1), {k: v for k, v in DEFAULTS.items() if k != "idle_level"},
               dict(DEFAULTS, night_level="10"), None]
        for settings in bad:
            with self.subTest(settings=settings), self.assertRaises(DisplaySettingsError):
                validate(settings)

    def test_store_persists_and_ignores_corrupt_files(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "display-settings.json"
            store = DisplaySettingsStore(path)
            self.assertEqual(store.settings, DEFAULTS)
            store.update(dict(DEFAULTS, night_level=5))
            self.assertEqual(DisplaySettingsStore(path).settings["night_level"], 5)
            path.write_text("{broken")
            self.assertEqual(DisplaySettingsStore(path).settings, DEFAULTS)

    def test_clock_message_carries_local_offset(self):
        message = clock_message(1790850000.7)
        self.assertEqual(message["epoch_s"], 1790850000)
        self.assertEqual(message["utc_offset_s"], time.localtime(1790850000).tm_gmtoff)


class BridgeAmbientTests(unittest.IsolatedAsyncioTestCase):
    async def test_hello_gets_clock_and_settings_only_with_capability(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = Bridge(WallpaperLibrary(Path(directory)))
            capable, legacy = FakeDevice(), FakeDevice()
            for device, caps in ((capable, ["ambient"]), (legacy, ["host_status"])):
                bridge.clients.add(device)
                bridge.send_locks[device] = asyncio.Lock()
                await bridge.handle_device_message(device, dict(HELLO, capabilities=caps))
            self.assertEqual([m["type"] for m in capable.sent], ["clock", "display_settings"])
            self.assertEqual(legacy.sent, [])
            result = await bridge.update_display_settings(dict(DEFAULTS, day_level=80))
            self.assertEqual(result["delivered_clients"], 1)
            self.assertEqual(capable.sent[-1]["day_level"], 80)
            with self.assertRaises(DisplaySettingsError):
                await bridge.update_display_settings(dict(DEFAULTS, day_level=0))
            self.assertEqual(bridge.display_settings.settings["day_level"], 80)

    def test_hello_with_many_capabilities_is_accepted(self):
        from bridge import parse_device_message
        caps = ["video_wallpaper", "host_status", "interactive_events", "events_v2",
                "agent_status", "ambient", "sd_wallpaper", "claude_companion", "ota"]
        parse_device_message(json.dumps(dict(HELLO, capabilities=caps)))


class DisplaySettingsHttpTests(unittest.TestCase):
    def test_get_and_put_are_local_and_validated(self):
        with tempfile.TemporaryDirectory() as temporary:
            def completed(value):
                future = Future(); future.set_result(value); return future
            store = DisplaySettingsStore(None)

            def write(settings):
                future = Future()
                try:
                    future.set_result({"settings": store.update(settings), "delivered_clients": 0})
                except ValueError as error:
                    future.set_exception(error)
                return future
            try:
                editor = WallpaperEditorService(WallpaperLibrary(Path(temporary)),
                    Path(__file__).resolve().parents[1], 0, lambda _: completed({}),
                    lambda: completed({}), lambda _: completed({}),
                    display=(store.message, write))
            except PermissionError:
                self.skipTest("loopback bind unavailable in sandbox")
            editor.start()
            try:
                url = f"http://127.0.0.1:{editor.port}/api/display-settings"
                with urlopen(url, timeout=3) as response:
                    self.assertEqual(json.load(response), DEFAULTS)
                body = json.dumps(dict(DEFAULTS, idle_dim_s=60)).encode()
                with urlopen(Request(url, data=body, method="PUT",
                                     headers={"Content-Type": "application/json"}), timeout=3) as r:
                    self.assertEqual(json.load(r)["settings"]["idle_dim_s"], 60)
                for data, headers, expected in (
                        (body, {"Content-Type": "application/json", "Origin": "https://evil.invalid"}, 403),
                        (body, {"Content-Type": "text/plain"}, 403),
                        (json.dumps(dict(DEFAULTS, idle_level=0)).encode(),
                         {"Content-Type": "application/json"}, 400)):
                    with self.subTest(headers=headers), self.assertRaises(HTTPError) as context:
                        urlopen(Request(url, data=data, method="PUT", headers=headers), timeout=3)
                    self.assertEqual(context.exception.code, expected)
                self.assertEqual(store.settings["idle_dim_s"], 60)
            finally:
                editor.stop()


if __name__ == "__main__":
    unittest.main()
