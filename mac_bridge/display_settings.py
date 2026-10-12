"""Screen clock and brightness settings shared with ESP32 devices.

The Bridge keeps one settings record in .runtime/display-settings.json, sends
it to devices advertising the ``ambient`` capability (with the Mac's time),
and devices store a copy in NVS so the rules also apply while offline.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import time
from typing import Any

AMBIENT_CAPABILITY = "ambient"
CLOCK_INTERVAL_SECONDS = 600.0

DEFAULTS: dict[str, int] = {
    "night_start_min": 23 * 60,
    "night_end_min": 7 * 60,
    "day_level": 100,
    "night_level": 10,
    "night_wake_level": 40,
    "idle_dim_s": 300,
    "idle_level": 30,
}


class DisplaySettingsError(ValueError):
    pass


def validate(settings: object) -> dict[str, int]:
    """Exactly the DEFAULTS keys, integers, and the firmware's invariants."""
    if not isinstance(settings, dict) or set(settings) != set(DEFAULTS):
        raise DisplaySettingsError("display settings need exactly the known fields")
    for key, value in settings.items():
        if type(value) is not int:
            raise DisplaySettingsError(f"{key} must be an integer")
    s = settings
    if not (0 <= s["night_start_min"] < 1440 and 0 <= s["night_end_min"] < 1440):
        raise DisplaySettingsError("night times must be minutes within a day")
    if not 5 <= s["day_level"] <= 100:
        raise DisplaySettingsError("day brightness must be 5-100")
    if not 1 <= s["night_level"] <= 100:
        raise DisplaySettingsError("night brightness must be 1-100")
    if not s["night_level"] <= s["night_wake_level"] <= 100:
        raise DisplaySettingsError("night wake brightness must be at least night brightness")
    if not 1 <= s["idle_level"] <= s["day_level"]:
        raise DisplaySettingsError("idle brightness must be 1 to the day brightness")
    if not 0 <= s["idle_dim_s"] <= 3600:
        raise DisplaySettingsError("idle delay must be 0-3600 seconds")
    return dict(settings)


class DisplaySettingsStore:
    def __init__(self, path: Path | None) -> None:
        self.path = path
        self.settings = dict(DEFAULTS)
        if path is not None and path.exists():
            try:
                self.settings = validate(json.loads(path.read_text(encoding="utf-8")))
            except (OSError, ValueError) as error:
                print(f"Display settings file ignored: {type(error).__name__}", flush=True)

    def update(self, settings: object) -> dict[str, int]:
        validated = validate(settings)
        if self.path is not None:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.path.with_suffix(".tmp")
            temporary.write_text(json.dumps(validated, indent=2), encoding="utf-8")
            os.replace(temporary, self.path)
        self.settings = validated
        return dict(validated)

    def message(self) -> dict[str, Any]:
        return {"v": 1, "type": "display_settings", **self.settings}


def clock_message(now: float | None = None) -> dict[str, Any]:
    """The Mac's wall-clock time and current UTC offset (follows its time zone)."""
    now = time.time() if now is None else now
    return {"v": 1, "type": "clock", "epoch_s": int(now),
            "utc_offset_s": int(time.localtime(now).tm_gmtoff)}
