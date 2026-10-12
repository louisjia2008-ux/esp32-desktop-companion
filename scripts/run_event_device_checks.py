#!/usr/bin/env python3
"""Drive the existing loopback event API; never synthesize a device touch.

This checks delivery/state transitions, duplicates and queue pressure. Visual
appearance, physical Dismiss/Retry and ten user choices still require a person.
Read the concurrently captured ESP32 serial log for device-side evidence.
"""

from __future__ import annotations
import argparse
import json
from pathlib import Path
import sys
import time
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mac_bridge"))
from bridge import action_required_message, notification_message


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor-port", type=int, default=8766)
    parser.add_argument("--duration", type=float, default=320)
    args = parser.parse_args()
    base = f"http://127.0.0.1:{args.editor_port}"
    started = time.monotonic()
    sequence = 0
    def post(packet):
        raw = json.dumps(packet, separators=(",", ":")).encode()
        with urlopen(Request(base + "/api/events", data=raw,
                            headers={"Content-Type": "application/json"}), timeout=12) as response:
            result = json.load(response)
        print(json.dumps({"elapsed_s": round(time.monotonic()-started, 2),
                          "sent": packet, "delivery": result}), flush=True)
        if result["delivered_clients"] < 1: raise RuntimeError("No event-capable device received packet")
    def cancel(identifier):
        post({"v":1, "type":"dismiss", "request_id": identifier})
    # Initial INFO and SUCCESS expire on the real device's clock.
    for level in ("info", "success"):
        packet = notification_message(level, f"probe-{level}")
        post(packet); time.sleep(5)
    # Durable warning/error remain until this explicit remote cancellation.
    for level in ("warning", "error"):
        packet = notification_message(level, f"probe-{level}")
        post(packet); time.sleep(5); cancel(packet["event_id"]); time.sleep(1)
    queued = []
    for level in ("info", "success"):
        packet = notification_message(level, f"burst-{level}")
        post(packet); queued.append(packet["event_id"])
    action = action_required_message("probe-queue-primary")
    post(action); post(action)  # Duplicate must not create another modal.
    queued.append(action["request_id"])
    packet = notification_message("error", "burst-error")
    post(packet); queued.append(packet["event_id"])
    for i in range(12):
        packet = notification_message("info", f"overflow-info-{i}")
        post(packet); queued.append(packet["event_id"])
    packet = action_required_message("probe-queue-secondary")
    post(packet); queued.insert(1, packet["request_id"])
    # Hold pressure long enough to capture at least one EVENT_METRICS sample.
    time.sleep(12)
    for identifier in queued: cancel(identifier); time.sleep(0.15)
    time.sleep(2)
    # Preserve substantial wallpaper time between notifications over >5 min.
    while time.monotonic() - started < args.duration:
        packet = notification_message("info" if sequence % 2 == 0 else "success", f"stability-{sequence}")
        packet["auto_dismiss_ms"] = 1000
        post(packet); sequence += 1
        time.sleep(min(12, max(0, args.duration - (time.monotonic() - started))))
    with urlopen(base + "/api/status", timeout=3) as response:
        status = json.load(response)
    print(json.dumps({"duration_s": round(time.monotonic() - started, 2),
                      "status": status, "physical_touches_synthesized": 0,
                      "physical_acceptance": "NOT_OBSERVED_BY_THIS_SCRIPT"}), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
