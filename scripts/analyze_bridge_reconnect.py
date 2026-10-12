#!/usr/bin/env python3
"""Correlate real serial JSONL with one or more pause_bridge JSONL journals.

This is an offline reader: it never opens serial, operates the Bridge, or creates
sensor measurements. UTC recovery times come from recorded timestamps, not from
uptime extrapolation. Missing observations remain NOT_TESTED. Its unit fixtures
are software tests and must never be presented as device evidence.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
from typing import Any


_SPEC = importlib.util.spec_from_file_location(
    "sensor_runtime_analysis", Path(__file__).with_name("analyze_sensor_runtime.py"))
RUNTIME = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(RUNTIME)
OBSERVATION_GAP_SECONDS = 30.0
TCP_SYNC_TIMEOUT_MS = 1500
FAULT_PREFIXES = ("CAPTURE_FAULT", "CAPTURE_IO_ERROR", "CAPTURE_LINE_OVERFLOW")
TRANSPORT_COUNTERS = ("tcp_attempts", "tcp_failures", "upgrades", "upgrade_timeouts",
                      "protocol_failures", "fresh_restarts", "heartbeat_resets",
                      "connections", "disconnects", "log_drops")


def combined_status(statuses: list[str]) -> str:
    if "FAIL" in statuses:
        return "FAIL"
    return "PASS" if statuses and all(value == "PASS" for value in statuses) else "NOT_TESTED"


def utc_seconds(value: Any) -> float | None:
    if not isinstance(value, str) or not value:
        return None
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise ValueError("UTC timestamps must include a timezone")
    return parsed.timestamp()


def utc_text(seconds: float | None) -> str | None:
    return datetime.fromtimestamp(seconds, timezone.utc).isoformat() if seconds is not None else None


def evidence(row: dict[str, Any] | None) -> dict[str, Any] | None:
    if row is None:
        return None
    # Keep fields needed for correlation; peer addresses and arbitrary packet
    # contents do not belong in this derived report.
    result = {key: row[key] for key in ("source_line", "elapsed_s", "utc", "journal") if key in row}
    result["kind"] = row.get("_prefix", row.get("action"))
    if "_fields" in row:
        result["fields"] = row["_fields"]
    return result


def prepare_capture(raw_records: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    records, issues = [], []
    previous_utc = previous_elapsed = None
    for index, raw in enumerate(raw_records, 1):
        row = {**raw, "source_line": raw.get("source_line", index)}
        row["_prefix"] = row["line"].split(" ", 1)[0].rstrip(";")
        row["_fields"] = {key: RUNTIME.number(value) for key, value in RUNTIME.FIELDS.findall(row["line"])}
        try:
            row["_utc"] = utc_seconds(row.get("utc"))
        except (ValueError, OverflowError) as error:
            row["_utc"] = None
            issues.append({"status": "FAIL", "kind": "invalid_capture_utc", "source_line": row["source_line"], "detail": str(error)})
        if row["_utc"] is None:
            issues.append({"status": "NOT_TESTED", "kind": "missing_capture_utc", "source_line": row["source_line"]})
        elif previous_utc is not None and row["_utc"] < previous_utc:
            issues.append({"status": "FAIL", "kind": "capture_utc_regression", "source_line": row["source_line"]})
        if previous_elapsed is not None and row["elapsed_s"] < previous_elapsed:
            issues.append({"status": "FAIL", "kind": "capture_elapsed_regression", "source_line": row["source_line"]})
        if row["_utc"] is not None:
            previous_utc = row["_utc"]
        previous_elapsed = row["elapsed_s"]
        records.append(row)
    return records, issues


def load_pause_journals(paths: list[Path]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    records, errors = [], []
    for path in paths:
        previous_utc = None
        with path.open(encoding="utf-8") as source:
            for index, line in enumerate(source, 1):
                try:
                    row = json.loads(line)
                    if not isinstance(row, dict) or not isinstance(row.get("action"), str):
                        raise ValueError("expected an object with a string action")
                    if row["action"] not in ("BRIDGE_PAUSED", "BRIDGE_RESUMED"):
                        continue  # SIGSTOP_REQUESTED isn't confirmation of a pause.
                    stamp = utc_seconds(row.get("utc"))
                    if stamp is None:
                        errors.append({"status": "NOT_TESTED", "kind": "missing_journal_utc", "journal": str(path), "source_line": index})
                    elif previous_utc is not None and stamp < previous_utc:
                        errors.append({"status": "FAIL", "kind": "journal_utc_regression", "journal": str(path), "source_line": index})
                    if stamp is not None:
                        previous_utc = stamp
                    records.append({**row, "source_line": index, "journal": str(path)})
                except (ValueError, OverflowError) as error:
                    errors.append({"status": "FAIL", "kind": "journal_parse_error", "journal": str(path), "source_line": index, "detail": str(error)})
    return records, errors


def pause_intervals(events: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    timed, intervals, issues = [], [], []
    for index, raw in enumerate(events, 1):
        if raw.get("action") not in ("BRIDGE_PAUSED", "BRIDGE_RESUMED"):
            continue
        row = {**raw, "source_line": raw.get("source_line", index)}
        try:
            row["_utc"] = utc_seconds(row.get("utc"))
        except (ValueError, OverflowError) as error:
            issues.append({"status": "FAIL", "kind": "invalid_journal_utc", "source_line": row["source_line"], "detail": str(error)})
            continue
        if row["_utc"] is None:
            issues.append({"status": "NOT_TESTED", "kind": "missing_journal_utc", "source_line": row["source_line"]})
            continue
        timed.append(row)
    # Different journal arguments may be supplied in any order. Within-file
    # regression is checked by load_pause_journals; overlapping pauses fail.
    timed.sort(key=lambda row: row["_utc"])
    pending = None
    for row in timed:
        if row["action"] == "BRIDGE_PAUSED":
            if pending is not None:
                issues.append({"status": "FAIL", "kind": "overlapping_or_unresumed_pause", "source_line": row["source_line"]})
                intervals.append({"paused": pending, "resumed": None})
            pending = row
        elif pending is None:
            issues.append({"status": "FAIL", "kind": "resume_without_pause", "source_line": row["source_line"]})
        else:
            if row.get("pid") is not None and pending.get("pid") is not None and row["pid"] != pending["pid"]:
                issues.append({"status": "FAIL", "kind": "pause_resume_pid_mismatch", "source_line": row["source_line"]})
            intervals.append({"paused": pending, "resumed": row})
            pending = None
    if pending is not None:
        intervals.append({"paused": pending, "resumed": None})
    return intervals, issues


def sensor_continuity(records: list[dict[str, Any]], prefix: str, pause_at: float,
                      resume_at: float, recovery_at: float, end_at: float) -> dict[str, Any]:
    rows = [row for row in records if row["_prefix"] == prefix and row["_utc"] is not None]
    before = next((row for row in reversed(rows) if row["_utc"] <= pause_at), None)
    after = next((row for row in rows if recovery_at <= row["_utc"] < end_at), None)
    result = {"status": "NOT_TESTED", "before": evidence(before), "after": evidence(after),
              "pause_observations": sum(pause_at <= row["_utc"] < resume_at for row in rows),
              "counter_deltas": {}, "issues": []}
    issues = result["issues"]
    state_key = "radar" if prefix == "SENSOR_RADAR" else "air"
    observation_end = after["_utc"] if after else recovery_at
    for row in records:
        if (row["_prefix"] == "SENSOR_STATE" and row["_utc"] is not None and
                pause_at <= row["_utc"] <= observation_end and row["_fields"].get(state_key) not in (None, "HEALTHY")):
            issues.append({"status": "FAIL", "kind": "logged_sensor_fault_state", "source_line": row["source_line"]})
    if before is None or after is None:
        issues.append({"status": "NOT_TESTED", "kind": "missing_before_or_after_sensor_observation"})
        result["status"] = combined_status([item["status"] for item in issues])
        return result
    scoped = [row for row in rows if before["_utc"] <= row["_utc"] <= after["_utc"]]
    gaps = [b["_utc"] - a["_utc"] for a, b in zip(scoped, scoped[1:])]
    result["maximum_observation_gap_s"] = max(gaps, default=0)
    if (pause_at - before["_utc"] > OBSERVATION_GAP_SECONDS or
            after["_utc"] - recovery_at > OBSERVATION_GAP_SECONDS or
            max(gaps, default=0) > OBSERVATION_GAP_SECONDS or not result["pause_observations"]):
        issues.append({"status": "NOT_TESTED", "kind": "sensor_observation_gap"})
    counters = RUNTIME.RADAR_COUNTERS if prefix == "SENSOR_RADAR" else RUNTIME.AIR_COUNTERS
    progress = "frames" if prefix == "SENSOR_RADAR" else "samples"
    age_limit = 2000 if prefix == "SENSOR_RADAR" else 15000
    for row in scoped:
        fields = row["_fields"]
        if "health" not in fields or "age_ms" not in fields:
            issues.append({"status": "NOT_TESTED", "kind": "incomplete_sensor_health", "source_line": row["source_line"]})
        elif fields["health"] != "HEALTHY" or not isinstance(fields["age_ms"], int) or not 0 <= fields["age_ms"] <= age_limit:
            issues.append({"status": "FAIL", "kind": "sensor_not_fresh_healthy", "source_line": row["source_line"]})
        if prefix == "SENSOR_AIR" and not RUNTIME.valid_air_reading(fields):
            issues.append({"status": "FAIL", "kind": "invalid_air_reading", "source_line": row["source_line"]})
    for key in counters:
        values = [row["_fields"].get(key) for row in scoped]
        if any(not isinstance(value, int) or isinstance(value, bool) for value in values):
            issues.append({"status": "NOT_TESTED", "kind": "missing_sensor_counter", "counter": key})
            result["counter_deltas"][key] = None
            continue
        deltas = [RUNTIME.counter_delta(a, b) for a, b in zip(values, values[1:])]
        if any(value is None for value in deltas):
            issues.append({"status": "FAIL", "kind": "sensor_counter_regression", "counter": key})
            result["counter_deltas"][key] = None
        else:
            result["counter_deltas"][key] = sum(deltas)
    if result["counter_deltas"].get(progress) == 0:
        issues.append({"status": "FAIL", "kind": "sensor_counter_not_advancing", "counter": progress})
    result["status"] = combined_status([item["status"] for item in issues]) if issues else "PASS"
    return result


def analyze_interval(interval: dict[str, Any], next_pause_at: float | None,
                     records: list[dict[str, Any]], bound: float) -> dict[str, Any]:
    paused, resumed = interval["paused"], interval["resumed"]
    pause_at = paused["_utc"]
    result = {"status": "NOT_TESTED", "paused": evidence(paused), "resumed": evidence(resumed),
              "pause_duration_s": resumed["_utc"] - pause_at if resumed else None,
              "next_pause_utc": utc_text(next_pause_at), "recovery_bound_seconds": bound, "issues": []}
    if resumed is None:
        result["issues"].append({"status": "NOT_TESTED", "kind": "pause_has_no_resume_record"})
        return result
    resume_at = resumed["_utc"]
    end_at = next_pause_at if next_pause_at is not None else float("inf")
    candidates = [row for row in records if row["_utc"] is not None and resume_at <= row["_utc"] < end_at]
    connected = next((row for row in candidates if row["line"].startswith("WebSocket connected")), None)
    # Complete the three steps on one connection. A close or another opening
    # clears pairing proof so an old answer cannot validate a new connection.
    session_connection = session_pairing = pairing = host = None
    for row in candidates:
        if row["line"].startswith("WebSocket connected"):
            session_connection, session_pairing = row, None
        elif row["line"].startswith("WebSocket disconnected"):
            session_connection = session_pairing = None
        elif row["line"].startswith("Bridge pairing challenge answered") and session_connection is not None:
            session_pairing = row
            if pairing is None:
                pairing = row
        elif row["_prefix"] == "HOST_STATUS_ACCEPTED" and session_connection is not None and session_pairing is not None:
            pairing, host = session_pairing, row
            break
    for name, row in (("connected", connected), ("pairing_answered", pairing), ("host_status_accepted", host)):
        latency = row["_utc"] - resume_at if row else None
        status = "NOT_TESTED" if row is None else ("PASS" if latency <= bound else "FAIL")
        result[name] = {"status": status, "latency_s": latency, "observation": evidence(row)}
    result["completed_session_connection"] = evidence(session_connection) if host else None
    until = host["_utc"] if host else min(end_at, max((row["_utc"] for row in records if row["_utc"] is not None), default=resume_at))
    scoped = [row for row in records if row["_utc"] is not None and pause_at <= row["_utc"] <= until and row["_utc"] < end_at]
    boot_rows = [evidence(row) for row in scoped if row["_prefix"] == "BOOT_METRICS"]
    event_before = next((row for row in reversed(records) if row["_prefix"] == "EVENT_METRICS" and row["_utc"] is not None and row["_utc"] < pause_at), None)
    boot_values = [row["_fields"].get("boot_count") for row in ([event_before] if event_before else []) + scoped if row["_prefix"] == "EVENT_METRICS"]
    boot_values = [value for value in boot_values if isinstance(value, int)]
    uptimes = [row["_fields"].get("uptime_ms") for row in scoped if row["_prefix"] == "SENSOR_METRICS"]
    uptime_reset = any(RUNTIME.counter_delta(a, b) is None for a, b in zip(uptimes, uptimes[1:]) if isinstance(a, int) and isinstance(b, int))
    rebooted = bool(boot_rows) or len(set(boot_values)) > 1 or uptime_reset
    result["no_reboot_during_recovery"] = {"status": "FAIL" if rebooted else ("PASS" if boot_values and host else "NOT_TESTED"),
                                          "boot_markers": boot_rows, "observed_boot_count_values": sorted(set(boot_values)), "uptime_reset": uptime_reset}
    faults = [evidence(row) for row in scoped if row["_prefix"].startswith(FAULT_PREFIXES)]
    result["capture_faults"] = faults
    if faults:
        result["issues"].append({"status": "FAIL", "kind": "capture_fault_during_interval"})
    wifi_rows = [row for row in scoped if row["_prefix"] == "METRICS"]
    wifi_states = [row["_fields"].get("wifi") for row in wifi_rows]
    wifi_times = [pause_at] + [row["_utc"] for row in wifi_rows] + [until]
    wifi_max_gap = max([0.0] + [b - a for a, b in zip(wifi_times, wifi_times[1:])])
    wifi_during_pause = sum(pause_at <= row["_utc"] < resume_at for row in wifi_rows)
    wifi_status = ("FAIL" if any(state not in (None, "connected") for state in wifi_states) else
                   "PASS" if wifi_during_pause and wifi_max_gap <= OBSERVATION_GAP_SECONDS and all(state == "connected" for state in wifi_states) else "NOT_TESTED")
    result["wifi_observations"] = {"status": wifi_status, "reports": len(wifi_rows),
                                   "reports_while_paused": wifi_during_pause, "maximum_observation_gap_s": wifi_max_gap,
                                   "connected": wifi_states.count("connected"), "other_or_missing": len(wifi_rows) - wifi_states.count("connected")}
    late_disconnected = [evidence(row) for row in candidates if row["_prefix"] == "METRICS" and
                         row["_utc"] > resume_at + bound and (host is None or row["_utc"] < host["_utc"]) and row["_fields"].get("websocket") == "disconnected"]
    if late_disconnected:
        result["issues"].append({"status": "FAIL", "kind": "observed_disconnected_after_recovery_deadline", "first_observation": late_disconnected[0]})
    result["sensors"] = {name: sensor_continuity(records, prefix, pause_at, resume_at, host["_utc"] if host else resume_at, end_at)
                         for name, prefix in (("radar", "SENSOR_RADAR"), ("air", "SENSOR_AIR"))}
    statuses = [result[key]["status"] for key in ("connected", "pairing_answered", "host_status_accepted", "no_reboot_during_recovery", "wifi_observations")]
    statuses += [value["status"] for value in result["sensors"].values()] + [issue["status"] for issue in result["issues"]]
    result["status"] = combined_status(statuses)
    return result


def counter_summary(records: list[dict[str, Any]], prefix: str, names: tuple[str, ...]) -> dict[str, Any]:
    result = {}
    for name in names:
        values = [row["_fields"].get(name) for row in records if row["_prefix"] == prefix]
        summary = RUNTIME.stats(values)
        numeric = [value for value in values if isinstance(value, int)]
        deltas = [RUNTIME.counter_delta(a, b) for a, b in zip(numeric, numeric[1:])]
        summary["observed_increment"] = sum(deltas) if numeric and all(value is not None for value in deltas) else None
        summary["counter_regressions"] = sum(value is None for value in deltas)
        result[name] = summary
    return result


def _analyze(raw_records: list[dict[str, Any]], pause_events: list[dict[str, Any]],
             minimum_seconds: float, recovery_bound_seconds: float):
    if any(not math.isfinite(value) or value <= 0 for value in (minimum_seconds, recovery_bound_seconds)):
        raise ValueError("durations must be finite and positive")
    records, timestamp_issues = prepare_capture(raw_records)
    raw_utc_times = [row["_utc"] for row in records if row["_utc"] is not None]
    intervals, journal_issues = pause_intervals(pause_events)
    runtime, samples, series = RUNTIME.analyze_records(raw_records, minimum_seconds)
    # A USB-open reset may be preceded by buffered records from the previous
    # boot. Preserve raw duration/hash, but never reuse its counters or timing.
    first_boot = next((index for index, row in enumerate(records) if row["_prefix"] == "BOOT_METRICS"), None)
    if first_boot is not None:
        records = records[first_boot:]
    results = [analyze_interval(value, intervals[index + 1]["paused"]["_utc"] if index + 1 < len(intervals) else None,
                                records, recovery_bound_seconds) for index, value in enumerate(intervals)]
    transport = [row for row in records if row["_prefix"] == "WS_TRANSPORT"]
    polls = RUNTIME.stats([row["_fields"].get("max_poll_ms") for row in transport])
    capture_status = combined_status([issue["status"] for issue in timestamp_issues + journal_issues]) if timestamp_issues or journal_issues else ("PASS" if records else "NOT_TESTED")
    reconnect_status = combined_status([value["status"] for value in results] + [issue["status"] for issue in journal_issues])
    invalid_polls = [evidence(row) for row in transport if row["_fields"].get("max_poll_ms") is not None and
                     (not isinstance(row["_fields"]["max_poll_ms"], int) or row["_fields"]["max_poll_ms"] < 0)]
    transport_status = "FAIL" if invalid_polls else ("PASS" if polls["count"] else "NOT_TESTED")
    def observation_counts(field: str) -> dict[str, int]:
        result: dict[str, int] = {}
        for row in transport:
            if field in row["_fields"]:
                key = str(row["_fields"][field])
                result[key] = result.get(key, 0) + 1
        return result
    timed = [row["_utc"] for row in records if row["_utc"] is not None]
    report = {
        "schema_version": 1,
        "evidence_scope": "Recorded serial and pause-journal observations only. No generated readings, electrical certification, precision guarantee, or physical UI confirmation.",
        "status": combined_status([capture_status, reconnect_status, runtime["healthy_window"]["status"], transport_status]),
        "reconnect_status": reconnect_status,
        "input_integrity": {"status": capture_status, "issues": timestamp_issues + journal_issues},
        "capture": {**runtime["capture"], "raw_utc_duration_s": raw_utc_times[-1] - raw_utc_times[0] if raw_utc_times else None,
                    "first_utc": utc_text(raw_utc_times[0]) if raw_utc_times else None, "last_utc": utc_text(raw_utc_times[-1]) if raw_utc_times else None,
                    "first_analyzed_utc": utc_text(timed[0]) if timed else None},
        "healthy_window": runtime["healthy_window"],
        "pauses": results,
        "transport": {"status": transport_status, "reports": len(transport), "max_poll_ms": polls, "invalid_timing_observations": invalid_polls,
                      "configured_synchronous_tcp_timeout_ms": TCP_SYNC_TIMEOUT_MS,
                      "limit_note": "1500ms bounds an underlying synchronous TCP operation. max_poll_ms is measured transport-task work; it does not measure display-loop latency.",
                      "timing": {key: RUNTIME.stats([row["_fields"].get(key) for row in transport]) for key in ("last_attempt_ms", "max_attempt_ms", "max_poll_ms", "max_loop_ms", "last_tcp_ms", "max_tcp_ms")},
                      "stage_report_counts": observation_counts("stage"),
                      "last_error_report_counts": observation_counts("last_error"),
                      "report_count_note": "Stage and last_error frequencies count logged snapshots, not distinct new events. heartbeat_resets records intentional heartbeat-state reinitialization, not a detected heartbeat failure.",
                      "counters": counter_summary(records, "WS_TRANSPORT", TRANSPORT_COUNTERS)},
        "distinct_error_counters": {
            "note": "Each counter is reported separately. Overlapping totals such as air.errors and air.crc are not added together. Transport failures during an intentional pause are expected observations, not sensor faults.",
            "radar": counter_summary(records, "SENSOR_RADAR", RUNTIME.RADAR_COUNTERS[1:]),
            "air": counter_summary(records, "SENSOR_AIR", RUNTIME.AIR_COUNTERS[1:]),
            "touch": counter_summary(records, "TOUCH_METRICS", ("read_errors", "forced_releases", "queue_overflows")),
            "transport": counter_summary(records, "WS_TRANSPORT", ("tcp_failures", "upgrade_timeouts", "protocol_failures", "log_drops")),
        },
        "radar": runtime["radar"], "air": runtime["air"], "runtime": runtime["runtime"],
        "physical_confirmation": "NOT_TESTED",
    }
    return report, runtime, samples, series


def analyze_records(raw_records: list[dict[str, Any]], pause_events: list[dict[str, Any]],
                    minimum_seconds: float = 1800, recovery_bound_seconds: float = 45) -> dict[str, Any]:
    return _analyze(raw_records, pause_events, minimum_seconds, recovery_bound_seconds)[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True, help="capture_sensor_runtime.py runtime.jsonl")
    parser.add_argument("--pause", type=Path, action="append", required=True, help="pause_bridge journal; may be repeated")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--minimum-seconds", type=float, default=1800)
    parser.add_argument("--recovery-bound-seconds", type=float, default=45)
    args = parser.parse_args()
    if any(not math.isfinite(value) or value <= 0 for value in (args.minimum_seconds, args.recovery_bound_seconds)):
        parser.error("durations must be finite and positive")
    input_paths = [("capture", args.capture)] + [("pause", value) for value in args.pause]
    input_snapshots = [(role, path, path.read_bytes()) for role, path in input_paths]
    records, parse_errors = RUNTIME.load_records(args.capture)
    events, journal_errors = load_pause_journals(args.pause)
    report, runtime, samples, series = _analyze(records, events, args.minimum_seconds, args.recovery_bound_seconds)
    report["input_integrity"]["issues"] += [{"status": "FAIL", "kind": "capture_parse_error", **error} for error in parse_errors] + journal_errors
    if report["input_integrity"]["issues"]:
        report["input_integrity"]["status"] = combined_status([value["status"] for value in report["input_integrity"]["issues"]])
        report["status"] = combined_status([report["status"], report["input_integrity"]["status"]])
    fault_marker = args.capture.parent / "hardware-fault.txt"
    if fault_marker.exists():
        report["capture"]["fault_marker_file"] = str(fault_marker.resolve())
        report["status"] = runtime["healthy_window"]["status"] = report["healthy_window"]["status"] = "FAIL"
    report["inputs"] = []
    for role, path, snapshot in input_snapshots:
        stable = path.read_bytes() == snapshot
        report["inputs"].append({"role": role, "path": str(path.resolve()), "sha256": hashlib.sha256(snapshot).hexdigest(),
                                 "bytes": len(snapshot), "unchanged_during_analysis": stable})
        if not stable:
            report["input_integrity"]["issues"].append({"status": "FAIL", "kind": "input_changed_during_analysis", "path": str(path.resolve())})
            report["input_integrity"]["status"] = report["status"] = "FAIL"
    runtime["input"] = str(args.capture.resolve())
    runtime["capture"]["parse_errors"] = parse_errors
    if parse_errors:
        runtime["healthy_window"]["status"] = "FAIL"
    args.output.mkdir(parents=True, exist_ok=True)
    report_path = args.output / "reconnect-summary.json"
    report["runtime_summary_path"] = str((args.output / "runtime-summary.json").resolve())
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    (args.output / "runtime-summary.json").write_text(json.dumps(runtime, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    RUNTIME.write_csv(args.output / "actual-air-samples.csv", samples, ("sample_at_ms", "co2_ppm", "temperature_c", "humidity_rh"))
    RUNTIME.write_csv(args.output / "runtime-series.csv", series, ("elapsed_s", "uptime_ms", "radar_health", "air_health"))
    print(json.dumps({"status": report["status"], "reconnect_status": report["reconnect_status"], "pause_count": len(report["pauses"]),
                      "healthy_window_status": report["healthy_window"]["status"], "summary": str(report_path.resolve())}))
    return 1 if report["status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
