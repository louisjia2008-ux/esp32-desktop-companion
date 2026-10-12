#!/usr/bin/env python3
"""Offline correlation fixtures only. These values are never device evidence."""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/analyze_bridge_reconnect.py"
SPEC = importlib.util.spec_from_file_location("reconnect_analysis", SCRIPT)
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)
START = datetime(2026, 10, 11, 16, 0, tzinfo=timezone.utc)


def utc(at):
    return (START + timedelta(seconds=at)).isoformat()


def record(at, line):
    return dict(elapsed_s=at, utc=utc(at), line=line)


def journal(paused=20, resumed=70, label="offline-fixture"):
    return [dict(utc=utc(paused - .01), action="SIGSTOP_REQUESTED", pid=123, journal=label),
            dict(utc=utc(paused), action="BRIDGE_PAUSED", pid=123, journal=label),
            dict(utc=utc(resumed), action="BRIDGE_RESUMED", pid=123, journal=label)]


def fixture(pauses=((20, 70),), finish=185):
    rows = [record(0, "CAPTURE_OPENED; offline fixture only"),
            record(.1, "BOOT_METRICS boot_count=13 reset_reason=1"),
            record(1, "WebSocket connected"),
            record(2, "Bridge pairing challenge answered"),
            record(3, "HOST_STATUS_ACCEPTED cpu=20 memory=60 uptime_s=123")]
    for at in range(5, finish - 5, 10):
        disconnected = any(paused <= at < resumed + 3 for paused, resumed in pauses)
        rows += [
            record(at, "METRICS fps=8.3 free_heap=33000 free_psram=7190000 wifi=connected "
                   + "websocket=" + ("disconnected" if disconnected else "connected")
                   + " wallpaper=video backlight=100 window_ms=10000 frame_count=83"),
            record(at + .1, f"SENSOR_RADAR health=HEALTHY presence=YES age_ms=70 frames={1000+at*10} invalid=0 unsupported=0 uart_errors=0 moving_cm=55 still_cm=50 moving_energy=50 still_energy=40 stack_free=2700"),
            record(at + .2, f"SENSOR_AIR health=HEALTHY age_ms=1000 sample_at_ms={at*1000-1000} co2_ppm=1800 temperature_c=24.5 humidity_rh=45 samples={10+at//5} errors=0 nack=0 crc=0 short=0 invalid=0 recoveries=0 init_failures=0 stack_free=4500"),
            record(at + .3, f"SENSOR_METRICS uptime_ms={at*1000} min_heap=30000 largest_heap=18000 min_psram=7180000 largest_psram=6940000 log_drops=0 wake_override=1 awake=1 absent_ms=0 high_episode=0 high_ms=5000 page=0"),
            record(at + .4, f"EVENT_METRICS overlay=0 modal_frames=0 base_frames=83 uptime_ms={at*1000} boot_count=13 loop_stack_free=11000"),
            record(at + .5, f"TOUCH_METRICS samples={at*100} down_samples=3 read_errors=0 forced_releases=0 queue_overflows=0 max_gap_ms=16"),
            record(at + .6, f"WS_TRANSPORT stage=READY tcp_attempts=3 tcp_failures=1 upgrades=2 upgrade_timeouts=1 protocol_failures=0 fresh_restarts=1 heartbeat_resets=0 connections=2 disconnects=1 last_attempt_ms=5 max_attempt_ms=1500 max_poll_ms=1420 max_loop_ms=280 last_error=0 log_drops=0 uptime_ms={at*1000}"),
            record(at + .7, "NETWORK_DIAG WIFI_CONNECTION_EVENTS=1 WIFI_DISCONNECTION_EVENTS=0 WIFI_RECONNECTIONS=0 WS_CONNECTION_EVENTS=2 WS_DISCONNECTION_EVENTS=1 WS_RECONNECTIONS=1"),
        ]
    for paused, resumed in pauses:
        rows += [record(paused + .2, "WebSocket disconnected"),
                 record(resumed + 1, "WebSocket connected"),
                 record(resumed + 2, "Bridge pairing challenge answered"),
                 record(resumed + 3, "HOST_STATUS_ACCEPTED cpu=22 memory=61 uptime_s=456")]
    rows.append(record(finish, "CAPTURE_CLOSED"))
    return sorted(rows, key=lambda row: row["elapsed_s"])


def analyze(rows=None, events=None, minimum=120):
    return ANALYZER.analyze_records(rows if rows is not None else fixture(),
                                    events if events is not None else journal(),
                                    minimum_seconds=minimum, recovery_bound_seconds=45)


def edit(rows, prefix, at, before, after):
    for row in rows:
        if at <= row["elapsed_s"] < at + 1 and row["line"].startswith(prefix):
            row["line"] = row["line"].replace(before, after)


class ReconnectAnalysisTests(unittest.TestCase):
    def test_complete_ordered_recovery_measures_utc_and_sensor_progress(self):
        report = analyze()
        pause = report["pauses"][0]
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(pause["status"], "PASS")
        self.assertEqual(pause["pause_duration_s"], 50)
        self.assertEqual(pause["connected"]["latency_s"], 1)
        self.assertEqual(pause["pairing_answered"]["latency_s"], 2)
        self.assertEqual(pause["host_status_accepted"]["latency_s"], 3)
        self.assertEqual(pause["sensors"]["radar"]["counter_deltas"]["frames"], 600)
        self.assertEqual(pause["sensors"]["air"]["counter_deltas"]["samples"], 12)
        self.assertEqual(pause["wifi_observations"]["status"], "PASS")
        self.assertEqual(report["transport"]["max_poll_ms"]["max"], 1420)
        self.assertEqual(report["transport"]["configured_synchronous_tcp_timeout_ms"], 1500)
        self.assertEqual(report["capture"]["raw_capture_duration_s"], 185)
        self.assertEqual(report["capture"]["raw_utc_duration_s"], 185)
        self.assertEqual(report["physical_confirmation"], "NOT_TESTED")

    def test_boot_marker_with_unchanged_counter_cannot_false_pass(self):
        rows = fixture() + [record(40, "BOOT_METRICS boot_count=13 reset_reason=3")]
        report = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["pauses"][0]["no_reboot_during_recovery"]["status"], "FAIL")
        self.assertEqual(len(report["pauses"][0]["no_reboot_during_recovery"]["boot_markers"]), 1)

    def test_event_boot_change_without_boot_line_also_fails(self):
        rows = fixture()
        for row in rows:
            if row["elapsed_s"] >= 45:
                row["line"] = row["line"].replace("boot_count=13", "boot_count=14")
        self.assertEqual(analyze(rows)["pauses"][0]["no_reboot_during_recovery"]["status"], "FAIL")

    def test_multiple_pause_journals_are_chronological_and_independent(self):
        pauses = ((20, 70), (100, 150))
        report = analyze(fixture(pauses), journal(100, 150, "second") + journal(20, 70, "first"))
        self.assertEqual(report["reconnect_status"], "PASS")
        self.assertEqual([row["paused"]["utc"] for row in report["pauses"]], [utc(20), utc(100)])
        self.assertEqual([row["host_status_accepted"]["latency_s"] for row in report["pauses"]], [3, 3])

    def test_recovery_after_next_pause_cannot_validate_previous_interval(self):
        rows = [row for row in fixture(((20, 70), (100, 150)))
                if not 70 < row["elapsed_s"] < 74]
        report = analyze(rows, journal(20, 70) + journal(100, 150))
        self.assertEqual(report["pauses"][0]["connected"]["status"], "NOT_TESTED")
        self.assertEqual(report["pauses"][0]["host_status_accepted"]["status"], "NOT_TESTED")
        self.assertEqual(report["pauses"][1]["status"], "PASS")

    def test_missing_pairing_log_is_not_sensor_or_connection_success(self):
        rows = [row for row in fixture() if not (row["elapsed_s"] == 72)]
        pause = analyze(rows)["pauses"][0]
        self.assertEqual(pause["connected"]["status"], "PASS")
        self.assertEqual(pause["pairing_answered"]["status"], "NOT_TESTED")
        self.assertEqual(pause["host_status_accepted"]["status"], "NOT_TESTED")
        self.assertEqual(pause["status"], "NOT_TESTED")

    def test_missing_host_log_and_host_before_pairing_remain_not_tested(self):
        rows = [row for row in fixture() if row["elapsed_s"] != 73]
        rows.append(record(70.5, "HOST_STATUS_ACCEPTED cpu=22 memory=61 uptime_s=456"))
        pause = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))["pauses"][0]
        self.assertEqual(pause["pairing_answered"]["status"], "PASS")
        self.assertEqual(pause["host_status_accepted"]["status"], "NOT_TESTED")
        self.assertEqual(pause["status"], "NOT_TESTED")

    def test_pairing_from_closed_connection_cannot_validate_new_session(self):
        rows = fixture() + [record(72.5, "WebSocket disconnected"), record(72.7, "WebSocket connected")]
        pause = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))["pauses"][0]
        self.assertEqual(pause["host_status_accepted"]["status"], "NOT_TESTED")
        rows.append(record(74, "Bridge pairing challenge answered"))
        rows.append(record(75, "HOST_STATUS_ACCEPTED cpu=22 memory=61 uptime_s=456"))
        pause = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))["pauses"][0]
        self.assertEqual(pause["status"], "PASS")
        self.assertEqual(pause["host_status_accepted"]["latency_s"], 5)

    def test_late_connection_pairing_or_host_fails_measured_bound(self):
        for stage, offset in (("connected", 46), ("pairing_answered", 46), ("host_status_accepted", 46)):
            with self.subTest(stage=stage):
                rows = [row for row in fixture() if row["elapsed_s"] not in (71, 72, 73)]
                times = (116, 117, 118) if stage == "connected" else ((71, 116, 117) if stage == "pairing_answered" else (71, 72, 116))
                rows += [record(times[0], "WebSocket connected"), record(times[1], "Bridge pairing challenge answered"),
                         record(times[2], "HOST_STATUS_ACCEPTED cpu=22 memory=61 uptime_s=456")]
                report = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))
                self.assertEqual(report["pauses"][0][stage]["status"], "FAIL")
                self.assertEqual(report["pauses"][0]["status"], "FAIL")

    def test_observed_disconnected_after_deadline_fails_even_without_stage_logs(self):
        rows = [row for row in fixture() if row["elapsed_s"] not in (71, 72, 73)]
        edit(rows, "METRICS ", 125, "websocket=connected", "websocket=disconnected")
        pause = analyze(rows)["pauses"][0]
        self.assertEqual(pause["connected"]["status"], "NOT_TESTED")
        self.assertEqual(pause["status"], "FAIL")
        self.assertTrue(any(issue["kind"] == "observed_disconnected_after_recovery_deadline" for issue in pause["issues"]))

    def test_closed_capture_before_pause_cannot_claim_observation(self):
        report = analyze(fixture(finish=15))
        pause = report["pauses"][0]
        self.assertEqual(pause["status"], "NOT_TESTED")
        self.assertEqual(pause["wifi_observations"]["status"], "NOT_TESTED")
        self.assertEqual(pause["sensors"]["air"]["status"], "NOT_TESTED")
        self.assertEqual(report["healthy_window"]["status"], "NOT_TESTED")

    def test_missing_resume_stays_not_tested_and_overlapping_pause_is_invalid(self):
        self.assertEqual(analyze(events=journal()[:2])["pauses"][0]["status"], "NOT_TESTED")
        report = analyze(events=journal(20, 70) + journal(30, 80))
        self.assertEqual(report["reconnect_status"], "FAIL")
        self.assertTrue(any(issue["kind"] == "overlapping_or_unresumed_pause" for issue in report["input_integrity"]["issues"]))

    def test_wifi_disconnect_or_capture_fault_fails_interval(self):
        rows = fixture()
        edit(rows, "METRICS ", 45, "wifi=connected", "wifi=disconnected")
        self.assertEqual(analyze(rows)["pauses"][0]["wifi_observations"]["status"], "FAIL")
        rows = fixture() + [record(40, "CAPTURE_FAULT; unexpected boot or hardware fault")]
        report = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))
        self.assertEqual(report["pauses"][0]["status"], "FAIL")
        self.assertEqual(report["status"], "FAIL")

    def test_single_wifi_observation_does_not_prove_long_interval_coverage(self):
        rows = [row for row in fixture() if not (35 <= row["elapsed_s"] <= 75 and row["line"].startswith("METRICS "))]
        wifi = analyze(rows)["pauses"][0]["wifi_observations"]
        self.assertEqual(wifi["reports"], 1)
        self.assertEqual(wifi["status"], "NOT_TESTED")
        self.assertGreater(wifi["maximum_observation_gap_s"], 30)

    def test_transient_sensor_fault_and_counter_regression_fail(self):
        rows = fixture() + [record(40, "SENSOR_STATE radar=HEALTHY air=RECOVERING uptime_ms=40000")]
        self.assertEqual(analyze(sorted(rows, key=lambda row: row["elapsed_s"]))["pauses"][0]["sensors"]["air"]["status"], "FAIL")
        rows = fixture()
        edit(rows, "SENSOR_RADAR ", 45, "frames=1450", "frames=5")
        radar = analyze(rows)["pauses"][0]["sensors"]["radar"]
        self.assertEqual(radar["status"], "FAIL")
        self.assertIsNone(radar["counter_deltas"]["frames"])

    def test_stalled_sensor_or_large_missing_observation_gap_cannot_pass(self):
        rows = fixture()
        for row in rows:
            if row["line"].startswith("SENSOR_AIR "):
                row["line"] = re.sub(r"\bsamples=\d+", "samples=1", row["line"])
        self.assertEqual(analyze(rows)["pauses"][0]["sensors"]["air"]["status"], "FAIL")
        rows = [row for row in fixture() if not (20 < row["elapsed_s"] < 65 and row["line"].startswith("SENSOR_AIR "))]
        self.assertEqual(analyze(rows)["pauses"][0]["sensors"]["air"]["status"], "NOT_TESTED")

    def test_uint32_counter_wrap_is_progress_and_errors_are_not_double_counted(self):
        rows = fixture()
        for row in rows:
            if row["line"].startswith("SENSOR_RADAR "):
                before = 1000 + int(row["elapsed_s"]) * 10
                after = (0xFFFFFFFF - 200 + int(row["elapsed_s"]) * 10) & 0xFFFFFFFF
                row["line"] = row["line"].replace(f"frames={before}", f"frames={after}")
        report = analyze(rows)
        self.assertEqual(report["pauses"][0]["sensors"]["radar"]["status"], "PASS")
        self.assertEqual(report["pauses"][0]["sensors"]["radar"]["counter_deltas"]["frames"], 600)
        self.assertIn("crc", report["distinct_error_counters"]["air"])
        self.assertIn("errors", report["distinct_error_counters"]["air"])
        self.assertNotIn("total", report["distinct_error_counters"]["air"])

    def test_wallpaper_fps_reuses_modal_and_page_window_exclusion(self):
        rows = fixture()
        edit(rows, "EVENT_METRICS ", 45, "modal_frames=0", "modal_frames=5")
        edit(rows, "SENSOR_METRICS ", 65, "page=0", "page=13")
        report = analyze(rows)
        accepted = report["runtime"]["normal_wallpaper_windows"]
        self.assertNotIn(45, [row["elapsed_s"] for row in accepted])
        self.assertNotIn(65, [row["elapsed_s"] for row in accepted])
        self.assertNotIn(75, [row["elapsed_s"] for row in accepted])
        self.assertEqual(report["runtime"]["normal_wallpaper_fps"]["min"], 8.3)

    def test_missing_utc_is_not_tested_and_regressed_utc_fails(self):
        rows = fixture()
        rows[10].pop("utc")
        self.assertEqual(analyze(rows)["input_integrity"]["status"], "NOT_TESTED")
        rows = fixture()
        rows[10]["utc"] = utc(0)
        self.assertEqual(analyze(rows)["input_integrity"]["status"], "FAIL")

    def test_pre_boot_buffered_transport_counts_do_not_contaminate_new_boot(self):
        rows = fixture()
        rows[1]["elapsed_s"], rows[1]["utc"] = .5, utc(.5)
        rows.append(record(.2, "WS_TRANSPORT stage=READY tcp_attempts=999 tcp_failures=99 max_poll_ms=99999"))
        report = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))
        self.assertEqual(report["capture"]["pre_first_boot_records_ignored"], 2)
        self.assertEqual(report["transport"]["max_poll_ms"]["max"], 1420)
        self.assertEqual(report["transport"]["counters"]["tcp_failures"]["max"], 1)

    def test_before_resume_events_cannot_satisfy_recovery_and_request_is_not_pause(self):
        rows = [row for row in fixture() if row["elapsed_s"] not in (71, 72, 73)]
        rows += [record(60, "WebSocket connected"), record(61, "Bridge pairing challenge answered"),
                 record(62, "HOST_STATUS_ACCEPTED cpu=22 memory=61 uptime_s=456")]
        pause = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))["pauses"][0]
        self.assertEqual(pause["connected"]["status"], "NOT_TESTED")
        self.assertEqual(pause["host_status_accepted"]["status"], "NOT_TESTED")
        report = analyze(events=journal()[:1])
        self.assertEqual(report["pauses"], [])
        self.assertEqual(report["reconnect_status"], "NOT_TESTED")

    def test_observed_fault_wins_when_a_post_recovery_sensor_line_is_missing(self):
        rows = [row for row in fixture() if not (row["elapsed_s"] > 70 and row["line"].startswith("SENSOR_AIR "))]
        rows.append(record(40, "SENSOR_STATE radar=HEALTHY air=RECOVERING uptime_ms=40000"))
        air = analyze(sorted(rows, key=lambda row: row["elapsed_s"]))["pauses"][0]["sensors"]["air"]
        self.assertEqual(air["status"], "FAIL")
        self.assertIsNone(air["after"])

    def test_missing_transport_timing_is_not_tested_and_invalid_value_fails(self):
        rows = [row for row in fixture() if not row["line"].startswith("WS_TRANSPORT ")]
        self.assertEqual(analyze(rows)["transport"]["status"], "NOT_TESTED")
        rows = fixture()
        edit(rows, "WS_TRANSPORT ", 45, "max_poll_ms=1420", "max_poll_ms=-1")
        self.assertEqual(analyze(rows)["transport"]["status"], "FAIL")

    def test_current_transport_timing_schema_and_guard_classification(self):
        report = analyze()
        transport = report["transport"]
        self.assertEqual(transport["timing"]["max_attempt_ms"]["max"], 1500)
        self.assertEqual(transport["timing"]["max_loop_ms"]["max"], 280)
        self.assertEqual(transport["stage_report_counts"]["READY"], transport["reports"])
        self.assertEqual(transport["last_error_report_counts"]["0"], transport["reports"])
        self.assertIn("heartbeat_resets", transport["counters"])
        self.assertNotIn("heartbeat_resets", report["distinct_error_counters"]["transport"])

    def test_journal_clock_regression_is_reported_and_input_durations_are_validated(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "offline-fixture-journal.jsonl"
            path.write_text("".join(json.dumps(row) + "\n" for row in reversed(journal()[1:])), encoding="utf-8")
            _, errors = ANALYZER.load_pause_journals([path])
            self.assertTrue(any(issue["kind"] == "journal_utc_regression" for issue in errors))
        for value in (0, -1, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                ANALYZER.analyze_records(fixture(), journal(), minimum_seconds=value)

    def test_cli_reads_files_hashes_sources_and_does_not_mutate_them(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            capture = folder / "offline-fixture-capture.jsonl"
            pause = folder / "offline-fixture-pause.jsonl"
            capture.write_text("".join(json.dumps(row) + "\n" for row in fixture()), encoding="utf-8")
            pause.write_text("".join(json.dumps(row) + "\n" for row in journal()), encoding="utf-8")
            digest = hashlib.sha256(capture.read_bytes()).hexdigest()
            process = subprocess.run([sys.executable, str(SCRIPT), "--capture", str(capture), "--pause", str(pause),
                                      "--output", str(folder / "analysis"), "--minimum-seconds", "120"], capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            report = json.loads((folder / "analysis/reconnect-summary.json").read_text())
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["inputs"][0]["sha256"], digest)
            self.assertEqual(hashlib.sha256(capture.read_bytes()).hexdigest(), digest)
            self.assertTrue((folder / "analysis/runtime-summary.json").is_file())
            self.assertTrue((folder / "analysis/actual-air-samples.csv").is_file())
            self.assertTrue((folder / "analysis/runtime-series.csv").is_file())

    def test_malformed_journal_and_fault_marker_fail_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            capture, pause = folder / "capture.jsonl", folder / "pause.jsonl"
            capture.write_text("".join(json.dumps(row) + "\n" for row in fixture()), encoding="utf-8")
            pause.write_text("malformed\n" + "".join(json.dumps(row) + "\n" for row in journal()), encoding="utf-8")
            (folder / "hardware-fault.txt").write_text("offline fixture marker\n")
            process = subprocess.run([sys.executable, str(SCRIPT), "--capture", str(capture), "--pause", str(pause),
                                      "--output", str(folder / "analysis"), "--minimum-seconds", "120"], capture_output=True, text=True)
            self.assertEqual(process.returncode, 1, process.stderr)
            report = json.loads((folder / "analysis/reconnect-summary.json").read_text())
            self.assertEqual(report["status"], "FAIL")
            self.assertEqual(report["healthy_window"]["status"], "FAIL")
            self.assertTrue(any(issue["kind"] == "journal_parse_error" for issue in report["input_integrity"]["issues"]))


if __name__ == "__main__":
    unittest.main()
