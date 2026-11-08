"""Validate timing aggregation, unknown ALSA logs, and rejected corrupt data."""

import csv
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from tools import summarize as summary


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=str(Path(__file__).resolve().parent))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def rows(self, times=(10, 20, 30, 40, 100), backend="cpu", synthetic=1):
        return [
            {"block": i, "frames": 1024, "process_us": value, "deadline_us": "21333.333",
             "deadline_miss": 0, "running": 1, "direction": i % 2, "bypass": 0,
             "backend": backend, "synthetic": synthetic}
            for i, value in enumerate(times)
        ]

    def write_csv(self, rows, header=summary.CSV_FIELDS, name="blocks.csv"):
        path = self.root / name
        with path.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=header)
            writer.writeheader()
            writer.writerows(rows)
        return path

    def test_nearest_rank_is_not_interpolated(self):
        self.assertEqual(summary.nearest_rank([4, 1, 3, 2], 0.5), 2)
        self.assertEqual(summary.nearest_rank(list(range(1, 101)), 0.95), 95)
        self.assertEqual(summary.nearest_rank([7], 0.99), 7)
        with self.assertRaises(ValueError):
            summary.nearest_rank([], 0.5)

    def test_true_csv_counts_quantiles_and_tail_block_budget(self):
        rows = self.rows()
        rows[-1]["frames"] = 480
        rows[-1]["deadline_us"] = "10000.000"
        rows[-1]["deadline_miss"] = 1
        result = summary.summarize(self.write_csv(rows))
        self.assertEqual(result["blocks"], 5)
        self.assertEqual(result["frames"], 4 * 1024 + 480)
        self.assertEqual(result["audio_duration_seconds"], (4 * 1024 + 480) / 48000)
        self.assertEqual(result["backend"], "cpu")
        self.assertTrue(result["synthetic"])
        self.assertEqual({key: result["process_us"][key] for key in ("p50", "p95", "p99", "max")},
                         {"p50": 30, "p95": 100, "p99": 100, "max": 100})
        self.assertEqual(result["deadline_misses"], 1)
        self.assertEqual(result["block_budgets"], [
            {"frames": 480, "blocks": 1, "budget_us": 10000.0},
            {"frames": 1024, "blocks": 4, "budget_us": 21333.333},
        ])

    def test_omitted_logs_are_null_and_supplied_clean_log_is_zero(self):
        path = self.write_csv(self.rows())
        result = summary.summarize(path)
        self.assertIsNone(result["alsa_logs"]["capture"])
        self.assertIsNone(result["alsa_logs"]["playback"])
        clean = self.root / "clean.log"
        clean.write_text("Recording WAVE at 48000 Hz\n", encoding="utf-8")
        result = summary.summarize(path, capture_log=clean)
        self.assertEqual(result["alsa_logs"]["capture"]["reported_xrun_lines"], 0)
        self.assertIsNone(result["alsa_logs"]["playback"])

    def test_logs_count_lines_once_and_keyword_details_separately(self):
        log = self.root / "capture.log"
        log.write_text("overrun!!!\nUNDERRUN and XRUN\nclean line\nxrun_count=0\n", encoding="utf-8")
        result = summary.count_reported_xrun_lines(log)
        self.assertEqual(result, {"overrun_lines": 1, "underrun_lines": 1, "xrun_lines": 1, "reported_xrun_lines": 2})

    def test_rpe_scope_does_not_claim_isolated_dsp_timing(self):
        result = summary.summarize(self.write_csv(self.rows(backend="rpe", synthetic=0)))
        self.assertEqual(result["backend"], "rpe")
        self.assertFalse(result["synthetic"])
        self.assertIn("transport", result["performance_scope"])

    def test_rejects_empty_and_wrong_header(self):
        empty = self.root / "empty.csv"
        empty.write_text("", encoding="utf-8")
        for path in (empty, self.write_csv([]), self.write_csv([], header=("frames", "process_us"), name="bad.csv")):
            with self.subTest(path=path.name), self.assertRaises(ValueError):
                summary.summarize(path)

    def test_rejects_nonfinite_negative_invalid_and_mixed_rows(self):
        changes = [
            ("process_us", "nan"), ("process_us", "inf"), ("process_us", "-1"),
            ("process_us", "oops"), ("deadline_us", "nan"), ("deadline_us", "-1"),
            ("deadline_us", "23219.955"), ("frames", "-1"), ("frames", "0"),
            ("frames", "1024.0"), ("block", "-1"), ("block", "0"),
            ("deadline_miss", "2"), ("running", "3"), ("direction", "2"),
            ("bypass", "-1"), ("backend", "unknown"), ("backend", "rpe"),
            ("synthetic", "0"), ("synthetic", "2"),
        ]
        for field, value in changes:
            rows = self.rows()
            rows[1][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                summary.summarize(self.write_csv(rows))

    def test_rejects_missing_or_extra_cells(self):
        path = self.root / "bad_columns.csv"
        header = ",".join(summary.CSV_FIELDS) + "\n"
        for line in ("0,1024,1\n", "0,1024,1,21333.333,0,1,0,0,cpu,1,extra\n"):
            path.write_text(header + line, encoding="utf-8")
            with self.subTest(line=line), self.assertRaises(ValueError):
                summary.summarize(path)

    def test_cli_writes_json_without_personal_paths(self):
        source = self.write_csv(self.rows())
        output = self.root / "results" / "summary.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(summary.main([str(source), "--output", str(output)]), 0)
        text = output.read_text(encoding="utf-8")
        self.assertNotIn(str(self.root), text)
        self.assertNotIn(str(source), text)
        self.assertIsNone(json.loads(text)["alsa_logs"]["capture"])

    def test_cli_rejects_input_overwrite_and_missing_log(self):
        source = self.write_csv(self.rows())
        before = source.read_bytes()
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            summary.main([str(source), "--output", str(source)])
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(source.read_bytes(), before)
        with self.assertRaises(FileNotFoundError):
            summary.summarize(source, capture_log=self.root / "absent.log")


if __name__ == "__main__":
    unittest.main()
