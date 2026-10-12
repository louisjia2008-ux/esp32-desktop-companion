#!/usr/bin/env python3
"""Analyze captured v0.9 production evidence without generating sensor values.

The serial summary is normally printed every 10 seconds. A CSV row is the last
actual sample named by that report, not every approximately 5-second SCD41
measurement. A healthy window means consecutive fresh, healthy observations and
no logged fault/change; it is not independent electrical or accuracy evidence.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
from pathlib import Path
import re
from typing import Any


UINT32_MASK = 0xFFFFFFFF
PAIR_TOLERANCE_S = 1.0
FIELDS = re.compile(r"\b([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")
RADAR_COUNTERS = ("frames", "invalid", "unsupported", "uart_errors")
AIR_COUNTERS = ("samples", "errors", "nack", "crc", "short", "invalid", "recoveries", "init_failures")
NETWORK_COUNTERS = (
    "WIFI_CONNECTION_EVENTS", "WIFI_DISCONNECTION_EVENTS", "WIFI_RECONNECTIONS",
    "WS_CONNECTION_EVENTS", "WS_DISCONNECTION_EVENTS", "WS_RECONNECTIONS",
    "WIFI_OBSERVED_CONNECTIONS", "WIFI_OBSERVED_DISCONNECTIONS", "WIFI_OBSERVED_RECONNECTIONS",
    "WS_OBSERVED_CONNECTIONS", "WS_OBSERVED_DISCONNECTIONS", "WS_OBSERVED_RECONNECTIONS",
)


def number(value: str) -> Any:
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    try:
        result = float(value)
    except ValueError:
        return value
    return result if math.isfinite(result) else value


def stats(values: list[Any]) -> dict[str, Any]:
    values = [value for value in values if isinstance(value, (int, float)) and math.isfinite(value)]
    if not values:
        return {"count": 0, "min": None, "max": None, "first": None, "last": None}
    return {"count": len(values), "min": min(values), "max": max(values),
            "first": values[0], "last": values[-1]}


def counter_delta(previous: Any, current: Any) -> int | None:
    if not isinstance(previous, int) or not isinstance(current, int):
        return None
    if not 0 <= previous <= UINT32_MASK or not 0 <= current <= UINT32_MASK:
        return None
    if current >= previous:
        return current - previous
    if previous >= 0xF0000000 and current <= 0x0FFFFFFF:
        return (current - previous) & UINT32_MASK
    return None


def valid_air_reading(fields: dict[str, Any]) -> bool:
    return (isinstance(fields.get("co2_ppm"), int) and 0 < fields["co2_ppm"] <= 40000
            and isinstance(fields.get("temperature_c"), (int, float)) and math.isfinite(fields["temperature_c"])
            and -10 <= fields["temperature_c"] <= 60
            and isinstance(fields.get("humidity_rh"), (int, float)) and math.isfinite(fields["humidity_rh"])
            and 0 <= fields["humidity_rh"] <= 100)


def load_records(path: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    records, errors = [], []
    with path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            try:
                value = json.loads(line)
                if not isinstance(value, dict) or not isinstance(value.get("line"), str):
                    raise ValueError("expected an object with a string line")
                elapsed = value.get("elapsed_s")
                if not isinstance(elapsed, (int, float)) or not math.isfinite(elapsed) or elapsed < 0:
                    raise ValueError("elapsed_s must be finite and nonnegative")
                value["source_line"] = line_number
                records.append(value)
            except (ValueError, json.JSONDecodeError) as error:
                errors.append({"source_line": line_number, "error": str(error)})
    return records, errors


def analyze_records(raw_records: list[dict[str, Any]], minimum_seconds: float = 1800,
                    max_gap_seconds: float = 30) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, Any]]]:
    first_boot = next((index for index, row in enumerate(raw_records)
                       if row["line"].startswith("BOOT_METRICS ")), None)
    ignored_preamble = first_boot or 0
    total_raw_count = len(raw_records)
    total_raw_duration = (float(raw_records[-1]["elapsed_s"]) - float(raw_records[0]["elapsed_s"])) if raw_records else 0
    # USB open can deliver buffered post-flash output before causing the expected
    # initial reset. Those old counters/readings cannot extend this boot's run.
    if first_boot is not None:
        raw_records = raw_records[first_boot:]
    records: list[dict[str, Any]] = []
    by_prefix: dict[str, list[dict[str, Any]]] = {}
    times: dict[str, list[float]] = {}
    anomalies: list[dict[str, Any]] = []
    boot_values: list[int] = []
    boot_markers: list[dict[str, Any]] = []
    generation, current_boot = 0, None
    previous_elapsed = -1.0
    for index, raw in enumerate(raw_records):
        line = raw["line"]
        prefix = line.split(" ", 1)[0].rstrip(";")
        fields = {key: number(value) for key, value in FIELDS.findall(line)}
        elapsed = float(raw["elapsed_s"])
        if elapsed < previous_elapsed:
            anomalies.append({"kind": "capture_clock_regression", "source_line": raw.get("source_line", index + 1)})
        previous_elapsed = elapsed
        boot = fields.get("boot_count") if prefix in ("BOOT_METRICS", "EVENT_METRICS") else None
        if isinstance(boot, int):
            if boot not in boot_values:
                boot_values.append(boot)
            if current_boot is not None and boot != current_boot:
                generation += 1
            current_boot = boot
        value = {**raw, "_index": index, "_prefix": prefix, "_fields": fields,
                 "_elapsed": elapsed, "_generation": generation, "_boot": current_boot}
        records.append(value)
        by_prefix.setdefault(prefix, []).append(value)
        times.setdefault(prefix, []).append(elapsed)
        if prefix == "BOOT_METRICS":
            boot_markers.append({"elapsed_s": elapsed, **fields})

    # Normal capture input is monotonic. Avoid silently reordering corrupted
    # input; a reversed host timestamp makes evidence pairing unreliable.
    monotonic_capture = not any(value["kind"] == "capture_clock_regression" for value in anomalies)

    def nearest(prefix: str, elapsed: float, tolerance: float = PAIR_TOLERANCE_S) -> dict[str, Any] | None:
        if not monotonic_capture:
            return None
        values, positions = by_prefix.get(prefix, []), times.get(prefix, [])
        at = bisect.bisect_left(positions, elapsed)
        candidates = values[max(0, at - 1):at + 1]
        if not candidates:
            return None
        value = min(candidates, key=lambda item: abs(item["_elapsed"] - elapsed))
        return value if abs(value["_elapsed"] - elapsed) <= tolerance else None

    actual_air: list[dict[str, Any]] = []
    sample_keys: dict[tuple[int, int], dict[str, Any]] = {}
    for value in by_prefix.get("SENSOR_AIR", []):
        fields = value["_fields"]
        sample_at = fields.get("sample_at_ms")
        count = fields.get("samples")
        if (not isinstance(sample_at, int) or not 0 <= sample_at <= UINT32_MASK or
                not isinstance(count, int) or count <= 0 or not valid_air_reading(fields)):
            continue
        # A rejected measurement never updates these service fields. Keep
        # retained real readings, marking their observation health explicitly.
        key = (value["_generation"], sample_at)
        row = {"source_line": value.get("source_line", value["_index"] + 1),
               "elapsed_s": value["_elapsed"], "utc": value.get("utc", ""),
               "boot_generation": value["_generation"], "boot_count": value["_boot"],
               "health_at_log": fields.get("health"), "age_at_log_ms": fields.get("age_ms"),
               "sample_at_ms": sample_at, "cumulative_samples": fields["samples"],
               "co2_ppm": fields["co2_ppm"], "temperature_c": fields["temperature_c"],
               "humidity_rh": fields["humidity_rh"]}
        if key in sample_keys:
            previous = sample_keys[key]
            if any(previous[name] != row[name] for name in ("co2_ppm", "temperature_c", "humidity_rh")):
                anomalies.append({"kind": "conflicting_sample_values", "sample_at_ms": sample_at,
                                  "boot_generation": key[0]})
            continue
        sample_keys[key] = row
        actual_air.append(row)

    groups: list[dict[str, Any]] = []
    windows: list[dict[str, Any]] = []
    current_window = None
    previous = None
    last_uptime = None
    unwrapped_uptime = 0
    radar_intervals: list[dict[str, Any]] = []
    discontinuities: list[dict[str, Any]] = []
    sensor_states = by_prefix.get("SENSOR_STATE", [])
    for anchor in by_prefix.get("SENSOR_METRICS", []):
        fields, elapsed = anchor["_fields"], anchor["_elapsed"]
        radar = nearest("SENSOR_RADAR", elapsed)
        air = nearest("SENSOR_AIR", elapsed)
        runtime = nearest("METRICS", elapsed)
        event = nearest("EVENT_METRICS", elapsed)
        touch = nearest("TOUCH_METRICS", elapsed)
        rf = radar["_fields"] if radar else {}
        af = air["_fields"] if air else {}
        mf = runtime["_fields"] if runtime else {}
        ef = event["_fields"] if event else {}
        tf = touch["_fields"] if touch else {}
        uptime = fields.get("uptime_ms")
        delta_ms = counter_delta(last_uptime, uptime) if last_uptime is not None else 0
        same_generation = previous is None or previous["generation"] == anchor["_generation"]
        if previous is None or not same_generation or delta_ms is None:
            unwrapped_uptime = uptime if isinstance(uptime, int) else 0
        else:
            unwrapped_uptime += delta_ms
        valid_uptime = isinstance(uptime, int) and 0 <= uptime <= UINT32_MASK
        healthy = (valid_uptime and rf.get("health") == "HEALTHY" and af.get("health") == "HEALTHY"
                   and isinstance(rf.get("age_ms"), int) and 0 <= rf["age_ms"] <= 2000
                   and isinstance(af.get("age_ms"), int) and 0 <= af["age_ms"] <= 15000
                   and isinstance(rf.get("frames"), int) and rf["frames"] > 0
                   and isinstance(af.get("samples"), int) and af["samples"] > 0
                   and valid_air_reading(af)
                   and all(isinstance(rf.get(key), int) and 0 <= rf[key] <= UINT32_MASK for key in RADAR_COUNTERS)
                   and all(isinstance(af.get(key), int) and 0 <= af[key] <= UINT32_MASK for key in AIR_COUNTERS))
        group = {"elapsed_s": elapsed, "utc": anchor.get("utc", ""), "generation": anchor["_generation"],
                 "uptime_ms": uptime, "unwrapped_uptime_ms": unwrapped_uptime, "healthy": healthy,
                 "radar": rf, "air": af, "runtime": mf, "event": ef, "touch": tf, "sensor": fields}
        reasons = []
        if not healthy:
            reasons.append("missing_or_not_fresh_healthy_pair")
        if previous is not None:
            gap = elapsed - previous["elapsed_s"]
            if not same_generation:
                reasons.append("boot_generation_changed")
            if delta_ms is None or delta_ms == 0:
                reasons.append("device_uptime_reset_or_not_advancing")
                anomalies.append({"kind": "device_uptime_discontinuity", "elapsed_s": elapsed,
                                  "previous": last_uptime, "current": uptime})
            elif delta_ms > max_gap_seconds * 1000 or gap > max_gap_seconds or gap < 0:
                reasons.append("observation_gap_exceeds_limit")
            if same_generation:
                for name, keys in (("radar", RADAR_COUNTERS), ("air", AIR_COUNTERS)):
                    for key in keys:
                        before, after = previous[name].get(key), group[name].get(key)
                        if before is not None and after is not None and counter_delta(before, after) is None:
                            reasons.append(f"{name}_{key}_counter_regression")
                            anomalies.append({"kind": "sensor_counter_regression", "sensor": name,
                                              "counter": key, "elapsed_s": elapsed, "previous": before, "current": after})
                old_drops, new_drops = previous["sensor"].get("log_drops"), fields.get("log_drops")
                if isinstance(old_drops, int) and isinstance(new_drops, int) and new_drops > old_drops:
                    reasons.append("runtime_log_drops_increased")
                for state in sensor_states:
                    if previous["elapsed_s"] < state["_elapsed"] <= elapsed:
                        sf = state["_fields"]
                        if sf.get("radar") != "HEALTHY" or sf.get("air") != "HEALTHY":
                            reasons.append("logged_unhealthy_state_between_reports")
                            break
                frame_delta = counter_delta(previous["radar"].get("frames"), rf.get("frames"))
                if delta_ms is not None and 0 < delta_ms <= max_gap_seconds * 1000 and frame_delta is not None:
                    radar_intervals.append({"start_uptime_ms": previous["uptime_ms"], "end_uptime_ms": uptime,
                                            "elapsed_ms": delta_ms, "valid_frames": frame_delta,
                                            "frames_per_second": frame_delta * 1000 / delta_ms})
        if reasons:
            discontinuities.append({"elapsed_s": elapsed, "uptime_ms": uptime, "reasons": sorted(set(reasons))})
            if current_window is not None:
                windows.append(current_window)
                current_window = None
        if healthy:
            if current_window is None:
                current_window = {"start_elapsed_s": elapsed, "end_elapsed_s": elapsed,
                                  "start_uptime_ms": uptime, "end_uptime_ms": uptime,
                                  "duration_ms": 0, "duration_s": 0, "observations": 1,
                                  "start_radar_frames": rf["frames"], "end_radar_frames": rf["frames"],
                                  "start_air_samples": af["samples"], "end_air_samples": af["samples"]}
            else:
                current_window["end_elapsed_s"] = elapsed
                current_window["end_uptime_ms"] = uptime
                current_window["duration_ms"] += delta_ms
                current_window["duration_s"] = current_window["duration_ms"] / 1000
                current_window["observations"] += 1
                current_window["end_radar_frames"] = rf["frames"]
                current_window["end_air_samples"] = af["samples"]
        groups.append(group)
        previous, last_uptime = group, uptime
    if current_window is not None:
        windows.append(current_window)

    max_window = max(windows, key=lambda window: window["duration_s"]) if windows else None
    capture_faults = [{"elapsed_s": value["_elapsed"], "line": value["line"]} for value in records
                      if value["_prefix"] in ("CAPTURE_FAULT", "CAPTURE_IO_ERROR", "CAPTURE_LINE_OVERFLOW")]
    closed = bool(by_prefix.get("CAPTURE_CLOSED"))
    capture_duration = records[-1]["_elapsed"] - records[0]["_elapsed"] if records else 0
    unexpected_boots = max(0, len(boot_values) - 1, len(boot_markers) - 1)
    required_window_observed = max_window is not None and max_window["duration_s"] >= minimum_seconds
    window_status = "PASS" if required_window_observed else (
        "FAIL" if closed and capture_duration >= minimum_seconds else "NOT_TESTED")
    if capture_faults or unexpected_boots or anomalies:
        window_status = "FAIL"

    normal_fps, excluded_fps = [], []
    previous_runtime = None
    ui_transitions = [value for value in records if value["_prefix"] in ("PAGE", "GESTURE") or
                      value["_prefix"].startswith("EVENT_") and value["_prefix"] != "EVENT_METRICS"]
    for value in by_prefix.get("METRICS", []):
        mf, elapsed = value["_fields"], value["_elapsed"]
        anchor, event = nearest("SENSOR_METRICS", elapsed), nearest("EVENT_METRICS", elapsed)
        sf, ef = anchor["_fields"] if anchor else {}, event["_fields"] if event else {}
        # main.cpp resets modal_frames and base_frames after each report. They
        # describe this FPS window, unlike cumulative sensor/error counters.
        modal_frames = ef.get("modal_frames")
        window_start = elapsed - mf.get("window_ms", 0) / 1000 if isinstance(mf.get("window_ms"), int) else elapsed
        transition = any(window_start < row["_elapsed"] <= elapsed for row in ui_transitions)
        previous_page = nearest("SENSOR_METRICS", previous_runtime["_elapsed"]) if previous_runtime else None
        stayed_on_wallpaper = previous_page is None or previous_page["_fields"].get("page") == 0
        normal = (mf.get("wallpaper") == "video" and sf.get("page") == 0 and ef.get("overlay") == 0
                  and modal_frames == 0 and stayed_on_wallpaper and not transition)
        row = {"elapsed_s": elapsed, "fps": mf.get("fps"), "wallpaper": mf.get("wallpaper"),
               "page": sf.get("page"), "overlay": ef.get("overlay"), "modal_frames": modal_frames}
        (normal_fps if normal else excluded_fps).append(row)
        previous_runtime = value

    series = []
    dim_candidates, return_candidates, touch_candidates = [], [], []
    previous = None
    for group in groups:
        rf, af, mf, sf, ef, tf = (group[key] for key in ("radar", "air", "runtime", "sensor", "event", "touch"))
        row = {"elapsed_s": group["elapsed_s"], "uptime_ms": group["uptime_ms"],
               "radar_health": rf.get("health"), "presence": rf.get("presence"), "radar_frames": rf.get("frames"),
               "air_health": af.get("health"), "co2_ppm": af.get("co2_ppm"), "air_samples": af.get("samples"),
               "backlight": mf.get("backlight"), "page": sf.get("page"), "wake_override": sf.get("wake_override"),
               "awake": sf.get("awake"), "absent_ms": sf.get("absent_ms"), "high_episode": sf.get("high_episode"),
               "high_ms": sf.get("high_ms"), "overlay": ef.get("overlay"), "touch_down_samples": tf.get("down_samples"),
               "wifi": mf.get("wifi"), "websocket": mf.get("websocket"), "free_heap": mf.get("free_heap"),
               "free_psram": mf.get("free_psram"), "fps": mf.get("fps")}
        is_dim = (rf.get("health") == "HEALTHY" and rf.get("presence") == "NO" and sf.get("wake_override") == 1
                  and sf.get("awake") == 0 and isinstance(sf.get("absent_ms"), int) and sf["absent_ms"] >= 300000)
        if is_dim:
            dim_candidates.append(row)
        if previous is not None and previous.get("awake") == 0 and row.get("awake") == 1:
            brighter = isinstance(row.get("backlight"), int) and isinstance(previous.get("backlight"), int) and row["backlight"] > previous["backlight"]
            if row["presence"] == "YES" and row["wake_override"] == 1 and brighter:
                return_candidates.append({"before": previous, "after": row})
            touch_delta = counter_delta(previous.get("touch_down_samples"), row.get("touch_down_samples"))
            if row["presence"] == "NO" and row["wake_override"] == 1 and brighter and touch_delta is not None and touch_delta > 0:
                touch_candidates.append({"before": previous, "after": row})
        series.append(row)
        previous = row

    def prefix_stats(prefix: str, keys: tuple[str, ...]) -> dict[str, Any]:
        return {key: stats([value["_fields"].get(key) for value in by_prefix.get(prefix, [])]) for key in keys}

    radar_elapsed_ms = sum(value["elapsed_ms"] for value in radar_intervals)
    reminders = [{"elapsed_s": value["_elapsed"], "utc": value.get("utc", ""), **value["_fields"]}
                 for value in by_prefix.get("LOCAL_CO2_REMINDER", [])]
    report = {
        "schema_version": 1,
        "evidence_scope": "Serial observations of actual firmware; CSV contains logged last samples only. No electrical or accuracy verification.",
        "capture": {"record_count": len(records), "raw_record_count": total_raw_count,
                    "duration_s": capture_duration, "raw_capture_duration_s": total_raw_duration,
                    "pre_first_boot_records_ignored": ignored_preamble,
                    "first_analyzed_elapsed_s": records[0]["_elapsed"] if records else None, "closed": closed,
                    "faults": capture_faults, "boot_markers": boot_markers, "boot_count_values": boot_values,
                    "unexpected_boots_observed": unexpected_boots, "anomalies": anomalies},
        "healthy_window": {"status": window_status, "required_seconds": minimum_seconds,
                           "maximum_observation_gap_seconds": max_gap_seconds, "largest": max_window,
                           "windows": windows, "discontinuities": discontinuities},
        "radar": {"reports": len(by_prefix.get("SENSOR_RADAR", [])),
                  "last": by_prefix.get("SENSOR_RADAR", [{}])[-1].get("_fields", {}),
                  "counter_ranges": prefix_stats("SENSOR_RADAR", RADAR_COUNTERS),
                  "readings": prefix_stats("SENSOR_RADAR", ("moving_cm", "still_cm", "moving_energy", "still_energy", "age_ms")),
                  "device_interval_ms": radar_elapsed_ms,
                  "new_valid_frames_in_intervals": sum(value["valid_frames"] for value in radar_intervals),
                  "frames_per_second": sum(value["valid_frames"] for value in radar_intervals) * 1000 / radar_elapsed_ms if radar_elapsed_ms else None,
                  "stack_free_bytes": prefix_stats("SENSOR_RADAR", ("stack_free",))["stack_free"]},
        "air": {"reports": len(by_prefix.get("SENSOR_AIR", [])), "unique_logged_actual_samples": len(actual_air),
                "sample_capture_limit": "Reports approximately every 10 seconds skip intervening approximately 5-second readings; valid sample counters retain those measurements.",
                "last": by_prefix.get("SENSOR_AIR", [{}])[-1].get("_fields", {}),
                "readings": {key: stats([row[key] for row in actual_air]) for key in ("co2_ppm", "temperature_c", "humidity_rh")},
                "counter_ranges": prefix_stats("SENSOR_AIR", AIR_COUNTERS),
                "stack_free_bytes": prefix_stats("SENSOR_AIR", ("stack_free",))["stack_free"]},
        "runtime": {"memory": prefix_stats("METRICS", ("free_heap", "free_psram")),
                    "minimum_and_largest_blocks": prefix_stats("SENSOR_METRICS", ("min_heap", "largest_heap", "min_psram", "largest_psram", "log_drops")),
                    "loop_stack_free_bytes": prefix_stats("EVENT_METRICS", ("loop_stack_free",))["loop_stack_free"],
                    "normal_wallpaper_fps": stats([row["fps"] for row in normal_fps]),
                    "normal_wallpaper_windows": normal_fps, "other_ui_fps_windows": excluded_fps,
                    "touch": prefix_stats("TOUCH_METRICS", ("samples", "down_samples", "read_errors", "forced_releases", "queue_overflows", "max_gap_ms")),
                    "network": prefix_stats("NETWORK_DIAG", NETWORK_COUNTERS),
                    "network_connection_samples": {key: {state: sum(row["_fields"].get(key) == state for row in by_prefix.get("METRICS", []))
                                                          for state in ("connected", "disconnected")} for key in ("wifi", "websocket")}},
        "local_features": {"co2_reminder_events": reminders, "presence_dimming_observations": dim_candidates,
                           "presence_return_brightness_increases": return_candidates,
                           "touch_wake_brightness_increases": touch_candidates,
                           "physical_confirmation": "NOT_TESTED", "night_brightness_confirmation": "NOT_TESTED"},
    }
    return report, actual_air, series


def write_csv(path: Path, rows: list[dict[str, Any]], fallback_fields: tuple[str, ...]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]) if rows else fallback_fields,
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="capture_sensor_runtime.py runtime.jsonl")
    parser.add_argument("--output", type=Path, help="analysis directory; defaults beside input")
    parser.add_argument("--minimum-seconds", type=float, default=1800)
    parser.add_argument("--max-gap-seconds", type=float, default=30)
    args = parser.parse_args()
    if (not math.isfinite(args.minimum_seconds) or not math.isfinite(args.max_gap_seconds)
            or args.minimum_seconds <= 0 or args.max_gap_seconds <= 0):
        parser.error("durations must be positive")
    records, parse_errors = load_records(args.input)
    report, samples, series = analyze_records(records, args.minimum_seconds, args.max_gap_seconds)
    report["input"] = str(args.input.resolve())
    report["capture"]["parse_errors"] = parse_errors
    hardware_fault_file = args.input.parent / "hardware-fault.txt"
    if hardware_fault_file.exists():
        report["capture"]["fault_marker_file"] = str(hardware_fault_file.resolve())
        report["healthy_window"]["status"] = "FAIL"
    if parse_errors:
        report["healthy_window"]["status"] = "FAIL"
    output = args.output or args.input.parent / "analysis"
    output.mkdir(parents=True, exist_ok=True)
    (output / "runtime-summary.json").write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    write_csv(output / "actual-air-samples.csv", samples, ("sample_at_ms", "co2_ppm", "temperature_c", "humidity_rh"))
    write_csv(output / "runtime-series.csv", series, ("elapsed_s", "uptime_ms", "radar_health", "air_health"))
    largest = report["healthy_window"]["largest"]
    duration = largest["duration_s"] if largest else 0
    print(json.dumps({"healthy_window_status": report["healthy_window"]["status"],
                      "longest_healthy_seconds": duration, "unique_logged_air_samples": len(samples),
                      "unexpected_boots_observed": report["capture"]["unexpected_boots_observed"],
                      "summary": str((output / "runtime-summary.json").resolve())}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
