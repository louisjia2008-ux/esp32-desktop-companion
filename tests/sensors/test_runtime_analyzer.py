#!/usr/bin/env python3
"""Offline analyzer fixtures only; values here are never device evidence."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("runtime_analyzer", ROOT / "scripts/analyze_sensor_runtime.py")
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)


def record(at, line):
    return dict(elapsed_s=at, utc="offline-fixture", line=line)


def fixture(count=181, start_uptime=10000):
    rows = [record(0, "BOOT_METRICS boot_count=13 reset_reason=1")]
    for index in range(count):
        at = 1 + index * 10
        uptime = (start_uptime + index * 10000) & 0xFFFFFFFF
        sample_at = (uptime - 1000) & 0xFFFFFFFF
        rows += [
            record(at, "METRICS fps=8.0 free_heap=40000 free_psram=7000000 wifi=connected websocket=connected wallpaper=video backlight=100 window_ms=10000 frame_count=80"),
            record(at + .1, f"SENSOR_RADAR health=HEALTHY presence=YES age_ms=50 frames={100 + index*100} invalid=0 unsupported=0 uart_errors=0 moving_cm=50 still_cm=60 moving_energy=20 still_energy=30 stack_free=3000"),
            record(at + .2, f"SENSOR_AIR health=HEALTHY age_ms=1000 sample_at_ms={sample_at} co2_ppm={1650 + index%10} temperature_c=23.25 humidity_rh=45.5 samples={1 + index*2} errors=0 nack=0 crc=0 short=0 invalid=0 recoveries=0 init_failures=0 stack_free=4000"),
            record(at + .3, f"SENSOR_METRICS uptime_ms={uptime} min_heap=38000 largest_heap=20000 min_psram=7000000 largest_psram=6900000 log_drops=0 wake_override=1 awake=1 absent_ms=0 high_episode=1 high_ms=300000 page=0"),
            record(at + .4, f"EVENT_METRICS overlay=0 modal_frames=0 base_frames={80 + index*80} uptime_ms={uptime} boot_count=13 loop_stack_free=3500"),
            record(at + .5, f"TOUCH_METRICS samples={100 + index*100} down_samples=1 read_errors=0 forced_releases=0 queue_overflows=0 max_gap_ms=17"),
            record(at + .6, "NETWORK_DIAG WIFI_CONNECTION_EVENTS=1 WIFI_DISCONNECTION_EVENTS=0 WIFI_RECONNECTIONS=0 WS_CONNECTION_EVENTS=1 WS_DISCONNECTION_EVENTS=0 WS_RECONNECTIONS=0"),
        ]
    rows.append(record(count * 10 + 1, "CAPTURE_CLOSED"))
    return rows


def replace_group(rows, index, prefix, old, new):
    for row in rows:
        if 1 + index*10 <= row["elapsed_s"] < 2 + index*10 and row["line"].startswith(prefix + " "):
            row["line"] = row["line"].replace(old, new)


class RuntimeAnalyzerTests(unittest.TestCase):
    def test_healthy_device_uptime_window_and_unique_real_sample_rows(self):
        report, samples, series = ANALYZER.analyze_records(fixture())
        self.assertEqual(report["healthy_window"]["status"], "PASS")
        self.assertEqual(report["healthy_window"]["largest"]["duration_s"], 1800)
        self.assertEqual(report["radar"]["frames_per_second"], 10)
        self.assertEqual(len(samples), 181)
        self.assertEqual(samples[-1]["cumulative_samples"], 361)
        self.assertEqual(report["air"]["readings"]["co2_ppm"]["min"], 1650)
        self.assertEqual(report["runtime"]["normal_wallpaper_fps"]["count"], 181)
        self.assertEqual(len(series), 181)

    def test_large_missing_observation_gap_cannot_pass(self):
        rows = [row for row in fixture() if not 901 <= row["elapsed_s"] < 931]
        report, _, _ = ANALYZER.analyze_records(rows)
        self.assertEqual(report["healthy_window"]["status"], "FAIL")
        self.assertLess(report["healthy_window"]["largest"]["duration_s"], 1800)
        self.assertTrue(any("observation_gap_exceeds_limit" in gap["reasons"] for gap in report["healthy_window"]["discontinuities"]))

    def test_brief_logged_unhealthy_state_splits_window(self):
        rows = fixture() + [record(906, "SENSOR_STATE radar=STALE air=HEALTHY uptime_ms=915000")]
        rows.sort(key=lambda row: row["elapsed_s"])
        report, _, _ = ANALYZER.analyze_records(rows)
        self.assertEqual(report["healthy_window"]["status"], "FAIL")
        self.assertTrue(any("logged_unhealthy_state_between_reports" in gap["reasons"] for gap in report["healthy_window"]["discontinuities"]))

    def test_counter_regression_fails_even_with_later_healthy_rows(self):
        rows = fixture()
        replace_group(rows, 90, "SENSOR_RADAR", "frames=9100", "frames=10")
        report, _, _ = ANALYZER.analyze_records(rows)
        self.assertEqual(report["healthy_window"]["status"], "FAIL")
        self.assertTrue(any(value["kind"] == "sensor_counter_regression" for value in report["capture"]["anomalies"]))

    def test_reboot_counter_change_is_counted_without_inventing_more(self):
        rows = fixture()
        for row in rows:
            if row["elapsed_s"] >= 901:
                row["line"] = row["line"].replace("boot_count=13", "boot_count=14")
        rows.append(record(900, "BOOT_METRICS boot_count=14 reset_reason=3"))
        rows.sort(key=lambda row: row["elapsed_s"])
        report, _, _ = ANALYZER.analyze_records(rows)
        self.assertEqual(report["capture"]["boot_count_values"], [13, 14])
        self.assertEqual(report["capture"]["unexpected_boots_observed"], 1)
        self.assertEqual(report["healthy_window"]["status"], "FAIL")

    def test_deduplicate_retained_samples_by_sample_timestamp(self):
        rows = fixture(3)
        replace_group(rows, 1, "SENSOR_AIR", "sample_at_ms=19000", "sample_at_ms=9000")
        replace_group(rows, 1, "SENSOR_AIR", "co2_ppm=1651", "co2_ppm=1650")
        replace_group(rows, 1, "SENSOR_AIR", "samples=3", "samples=1")
        replace_group(rows, 1, "SENSOR_AIR", "age_ms=1000", "age_ms=11000")
        report, samples, _ = ANALYZER.analyze_records(rows, minimum_seconds=20)
        self.assertEqual(len(samples), 2)
        self.assertEqual(report["air"]["unique_logged_actual_samples"], 2)

    def test_normal_video_fps_excludes_modal_and_page_windows(self):
        rows = fixture(4)
        replace_group(rows, 1, "SENSOR_METRICS", "page=0", "page=13")
        replace_group(rows, 1, "EVENT_METRICS", "modal_frames=0", "modal_frames=5")
        replace_group(rows, 2, "EVENT_METRICS", "modal_frames=0", "modal_frames=5")
        report, _, _ = ANALYZER.analyze_records(rows, minimum_seconds=30)
        self.assertEqual(report["runtime"]["normal_wallpaper_fps"]["count"], 2)
        self.assertEqual(len(report["runtime"]["other_ui_fps_windows"]), 2)

    def test_equal_modal_window_counts_exclude_closing_window_then_recover(self):
        rows = fixture(4)
        replace_group(rows, 1, "EVENT_METRICS", "modal_frames=0", "modal_frames=5")
        replace_group(rows, 1, "EVENT_METRICS", "overlay=0", "overlay=1")
        replace_group(rows, 2, "EVENT_METRICS", "modal_frames=0", "modal_frames=5")
        # Same modal count in the closing window must still exclude its FPS,
        # despite overlay=0 at report time. The following modal_frames=0 window
        # is eligible immediately; it is not a cumulative-counter regression.
        report, _, _ = ANALYZER.analyze_records(rows, minimum_seconds=30)
        accepted = report["runtime"]["normal_wallpaper_windows"]
        excluded = report["runtime"]["other_ui_fps_windows"]
        self.assertEqual([row["elapsed_s"] for row in accepted], [1, 31])
        self.assertEqual([row["elapsed_s"] for row in excluded], [11, 21])
        self.assertEqual(excluded[1]["overlay"], 0)
        self.assertEqual(excluded[1]["modal_frames"], 5)

    def test_device_clock_and_counter_wrap_are_valid(self):
        rows = fixture(3, start_uptime=0xFFFFFFFF - 15000)
        for index in range(3):
            replace_group(rows, index, "SENSOR_RADAR", f"frames={100 + index*100}",
                          f"frames={(0xFFFFFFFF - 150 + index*100) & 0xFFFFFFFF}")
        report, _, _ = ANALYZER.analyze_records(rows, minimum_seconds=20)
        self.assertEqual(report["healthy_window"]["status"], "PASS")
        self.assertEqual(report["healthy_window"]["largest"]["duration_s"], 20)
        self.assertEqual(report["radar"]["frames_per_second"], 10)

    def test_fault_marker_fails_a_previously_healthy_capture(self):
        rows = fixture() + [record(1813, "CAPTURE_FAULT; disconnect power and inspect hardware")]
        report, _, _ = ANALYZER.analyze_records(rows)
        self.assertEqual(report["healthy_window"]["status"], "FAIL")
        self.assertEqual(len(report["capture"]["faults"]), 1)

    def test_local_feature_observations_do_not_claim_physical_confirmation(self):
        rows = fixture(5)
        for index in (1, 3):
            replace_group(rows, index, "SENSOR_RADAR", "presence=YES", "presence=NO")
            replace_group(rows, index, "SENSOR_METRICS", "awake=1 absent_ms=0", "awake=0 absent_ms=310000")
            replace_group(rows, index, "METRICS", "backlight=100", "backlight=30")
        replace_group(rows, 4, "SENSOR_RADAR", "presence=YES", "presence=NO")
        replace_group(rows, 4, "TOUCH_METRICS", "down_samples=1", "down_samples=6")
        rows += [record(26, "LOCAL_CO2_REMINDER shown=1 ppm=1652 duration_ms=12000 uptime_ms=35000")]
        rows.sort(key=lambda row: row["elapsed_s"])
        report, _, _ = ANALYZER.analyze_records(rows, minimum_seconds=40)
        local = report["local_features"]
        self.assertEqual(len(local["presence_dimming_observations"]), 2)
        self.assertEqual(len(local["presence_return_brightness_increases"]), 1)
        self.assertEqual(len(local["touch_wake_brightness_increases"]), 1)
        self.assertEqual(local["co2_reminder_events"][0]["ppm"], 1652)
        self.assertEqual(local["physical_confirmation"], "NOT_TESTED")

    def test_malformed_input_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.jsonl"
            path.write_text('{"elapsed_s":0,"line":"CAPTURE_CLOSED"}\ninvalid\n', encoding="utf-8")
            records, errors = ANALYZER.load_records(path)
            self.assertEqual(len(records), 1)
            self.assertEqual(errors[0]["source_line"], 2)

    def test_buffered_pre_first_boot_readings_do_not_extend_acceptance(self):
        rows = fixture(3)
        rows[0]["elapsed_s"] = .5
        preamble = [record(0, "SENSOR_RADAR health=HEALTHY presence=YES age_ms=50 frames=999999 invalid=0 unsupported=0 uart_errors=0"),
                    record(.1, "SENSOR_AIR health=HEALTHY age_ms=1000 sample_at_ms=9999 co2_ppm=9999 temperature_c=23 humidity_rh=45 samples=99999 errors=0 nack=0 crc=0 short=0 invalid=0 recoveries=0 init_failures=0")]
        report, samples, _ = ANALYZER.analyze_records(preamble + rows, minimum_seconds=20)
        self.assertEqual(report["capture"]["pre_first_boot_records_ignored"], 2)
        self.assertEqual(report["healthy_window"]["status"], "PASS")
        self.assertEqual(report["radar"]["counter_ranges"]["frames"]["max"], 300)
        self.assertEqual(len(samples), 3)
        self.assertLess(report["air"]["readings"]["co2_ppm"]["max"], 2000)

    def test_missing_pair_does_not_prove_a_continuous_window(self):
        rows = fixture(3)
        rows = [row for row in rows if not (11 <= row["elapsed_s"] < 12 and row["line"].startswith("SENSOR_AIR "))]
        report, _, _ = ANALYZER.analyze_records(rows, minimum_seconds=20)
        self.assertEqual(report["healthy_window"]["status"], "FAIL")
        self.assertEqual(report["healthy_window"]["largest"]["duration_s"], 0)

    def test_truncated_healthy_line_is_not_accepted(self):
        rows = fixture(3)
        for row in rows:
            if 11 <= row["elapsed_s"] < 12 and row["line"].startswith("SENSOR_AIR "):
                row["line"] = "SENSOR_AIR health=HEALTHY age_ms=0 samples=100"
        report, samples, _ = ANALYZER.analyze_records(rows, minimum_seconds=20)
        self.assertEqual(report["healthy_window"]["status"], "FAIL")
        self.assertEqual(len(samples), 2)

    def test_new_dropped_logs_split_a_healthy_window(self):
        rows = fixture(5)
        for index in range(2, 5):
            replace_group(rows, index, "SENSOR_METRICS", "log_drops=0", "log_drops=1")
        report, _, _ = ANALYZER.analyze_records(rows, minimum_seconds=40)
        self.assertEqual(report["healthy_window"]["status"], "FAIL")
        self.assertTrue(any("runtime_log_drops_increased" in gap["reasons"] for gap in report["healthy_window"]["discontinuities"]))


if __name__ == "__main__":
    unittest.main()
