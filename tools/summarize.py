#!/usr/bin/env python3
"""Summarize real per-block backend timing CSV, without claiming audio latency."""

import argparse
import csv
import json
import math
from pathlib import Path
import re
import sys


SAMPLE_RATE = 48000
CSV_FIELDS = (
    "block", "frames", "process_us", "deadline_us", "deadline_miss",
    "running", "direction", "bypass", "backend", "synthetic",
)
_INTEGER = re.compile(r"^[0-9]+$")
_XRUN_PATTERNS = {
    "overrun_lines": re.compile(r"\boverrun\b", re.IGNORECASE),
    "underrun_lines": re.compile(r"\bunderrun\b", re.IGNORECASE),
    "xrun_lines": re.compile(r"\bxrun\b", re.IGNORECASE),
}


def nearest_rank(values, probability):
    """Return sorted[ceil(probability * N) - 1], without interpolation."""
    if not values or not 0 < probability <= 1:
        raise ValueError("nearest-rank quantiles require samples and 0 < probability <= 1")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(probability * len(ordered)) - 1)]


def _integer(value, field, row_number, positive=False):
    if not isinstance(value, str) or not _INTEGER.fullmatch(value.strip()):
        raise ValueError("row {}: {} must be a nonnegative integer".format(row_number, field))
    result = int(value, 10)
    if positive and result == 0:
        raise ValueError("row {}: {} must be positive".format(row_number, field))
    return result


def _number(value, field, row_number, positive=False):
    try:
        result = float(value)
    except (ValueError, TypeError) as error:
        raise ValueError("row {}: {} must be numeric".format(row_number, field)) from error
    if not math.isfinite(result) or result < 0 or (positive and result == 0):
        raise ValueError("row {}: {} must be finite and {}".format(
            row_number, field, "positive" if positive else "nonnegative"))
    return result


def _flag(value, field, row_number):
    result = _integer(value, field, row_number)
    if result not in (0, 1):
        raise ValueError("row {}: {} must be 0 or 1".format(row_number, field))
    return result


def count_reported_xrun_lines(path):
    """Count matching log lines, not kernel events; an absent log is unknown."""
    if path is None:
        return None
    counts = {name: 0 for name in _XRUN_PATTERNS}
    counts["reported_xrun_lines"] = 0
    with Path(path).open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            matches = {name: bool(pattern.search(line)) for name, pattern in _XRUN_PATTERNS.items()}
            for name, matched in matches.items():
                counts[name] += int(matched)
            counts["reported_xrun_lines"] += int(any(matches.values()))
    return counts


def summarize(csv_path, capture_log=None, playback_log=None):
    """Validate the main.c CSV and summarize one backend and coefficient kind."""
    times = []
    frames = 0
    misses = 0
    backend = None
    synthetic = None
    previous_block = None
    budgets = {}
    try:
        with Path(csv_path).open("r", newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source, strict=True)
            if tuple(reader.fieldnames or ()) != CSV_FIELDS:
                raise ValueError("CSV header must be exactly: {}".format(",".join(CSV_FIELDS)))
            for row_number, row in enumerate(reader, 2):
                if None in row or any(row.get(field) is None for field in CSV_FIELDS):
                    raise ValueError("row {}: incorrect column count".format(row_number))
                block = _integer(row["block"], "block", row_number)
                block_frames = _integer(row["frames"], "frames", row_number, positive=True)
                process_us = _number(row["process_us"], "process_us", row_number)
                deadline_us = _number(row["deadline_us"], "deadline_us", row_number, positive=True)
                missed = _flag(row["deadline_miss"], "deadline_miss", row_number)
                for field in ("running", "direction", "bypass"):
                    _flag(row[field], field, row_number)
                row_synthetic = _flag(row["synthetic"], "synthetic", row_number)
                row_backend = row["backend"].strip()
                if row_backend not in ("cpu", "rpe"):
                    raise ValueError("row {}: backend must be cpu or rpe".format(row_number))
                if backend is not None and backend != row_backend:
                    raise ValueError("CSV mixes CPU and RPE backends; summarize each run separately")
                if synthetic is not None and synthetic != row_synthetic:
                    raise ValueError("CSV mixes synthetic and imported coefficient banks")
                if previous_block is not None and block <= previous_block:
                    raise ValueError("row {}: block numbers must strictly increase".format(row_number))
                expected_budget = block_frames * 1000000.0 / SAMPLE_RATE
                # main.c writes %.3f: accept rounding, but reject another rate.
                if not math.isclose(deadline_us, expected_budget, rel_tol=0, abs_tol=0.00051):
                    raise ValueError("row {}: deadline_us does not match {} frames at 48000 Hz".format(row_number, block_frames))
                previous_block = block
                backend, synthetic = row_backend, row_synthetic
                frames += block_frames
                misses += missed
                times.append(process_us)
                budgets[block_frames] = budgets.get(block_frames, 0) + 1
    except csv.Error as error:
        raise ValueError("invalid CSV: {}".format(error)) from error
    if not times:
        raise ValueError("CSV contains no processed audio blocks")
    return {
        "schema": "SPATIAL_BLOCK_SUMMARY_V1",
        "sample_rate": SAMPLE_RATE,
        "blocks": len(times),
        "frames": frames,
        "audio_duration_seconds": frames / SAMPLE_RATE,
        "backend": backend,
        "synthetic": bool(synthetic),
        "process_us": {
            "p50": nearest_rank(times, 0.50),
            "p95": nearest_rank(times, 0.95),
            "p99": nearest_rank(times, 0.99),
            "max": max(times),
            "quantile_method": "nearest-rank: sorted[ceil(p*N)-1]",
        },
        "deadline_misses": misses,
        "deadline_miss_source": "sum of deadline_miss flags emitted by the backend runner",
        "block_budgets": [
            {"frames": count, "blocks": budgets[count], "budget_us": round(count * 1000000.0 / SAMPLE_RATE, 3)}
            for count in sorted(budgets)
        ],
        "timing_scope": "Wall time of each backend_process call; excludes capture and playback buffering and is not end-to-end audio latency.",
        "performance_scope": (
            "CPU backend timing on the machine that produced the CSV; does not measure DSP performance."
            if backend == "cpu" else
            "RPE backend call timing includes host transport and synchronization; does not isolate DSP kernel execution time."
        ),
        "alsa_logs": {
            "capture": count_reported_xrun_lines(capture_log),
            "playback": count_reported_xrun_lines(playback_log),
            "reported_xrun_lines_definition": "Number of log lines containing overrun, underrun, or xrun as a word (case-insensitive); one line counts once even with multiple matches. This is not a complete driver event count. An omitted log is null, not zero.",
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", help="main.c per-block CSV from one run at 48000 Hz")
    parser.add_argument("--output", required=True, help="summary JSON path")
    parser.add_argument("--capture-log", help="optional arecord/ALSA stderr log")
    parser.add_argument("--playback-log", help="optional aplay/ALSA stderr log")
    args = parser.parse_args(argv)
    try:
        output = Path(args.output)
        inputs = [Path(path).resolve() for path in (args.csv, args.capture_log, args.playback_log) if path is not None]
        if output.resolve() in inputs:
            raise ValueError("summary output must not overwrite a CSV or ALSA log input")
        result = summarize(args.csv, args.capture_log, args.playback_log)
        encoded = json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded, encoding="utf-8")
    except (ValueError, OSError) as error:
        parser.exit(2, "error: {}\n".format(error))
    print("Wrote block timing summary: {}".format(output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
