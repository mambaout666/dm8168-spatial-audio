#!/usr/bin/env python3
"""Run the portable CPU demo with original audio and explicitly synthetic HRIR."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys

if __package__:
    from .audio_tools import generate_test_samples, make_demo_bank, pcm_to_wav, write_bank, write_raw_pcm, write_wav
else:
    from audio_tools import generate_test_samples, make_demo_bank, pcm_to_wav, write_bank, write_raw_pcm, write_wav


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", default="results/demo", help="output directory inside the project")
    parser.add_argument("--backend-binary", help="built spatial-audio executable")
    parser.add_argument("--duration", type=float, default=0.5)
    args = parser.parse_args(argv)
    try:
        output_dir = Path(args.output_dir)
        if not output_dir.is_absolute():
            output_dir = PROJECT_ROOT / output_dir
        output_dir = output_dir.resolve()
        try:
            output_dir.relative_to(PROJECT_ROOT)
        except ValueError as error:
            raise ValueError("demo output directory must stay inside the project") from error
        candidates = [PROJECT_ROOT / "build" / "spatial-audio", PROJECT_ROOT / "build" / "spatial-audio.exe"]
        binary = Path(args.backend_binary).resolve() if args.backend_binary else next((p for p in candidates if p.is_file()), candidates[0])
        if not binary.is_file():
            raise ValueError("backend executable not found: {}; build the C backend first".format(binary))
        samples = generate_test_samples(duration=args.duration)
        silence = generate_test_samples(duration=args.duration, silence=True)
        output_dir.mkdir(parents=True, exist_ok=True)
        write_raw_pcm(output_dir / "input.pcm", samples)
        write_wav(output_dir / "input.wav", samples)
        write_raw_pcm(output_dir / "silence_input.pcm", silence)
        bank_path = output_dir / "synthetic.hrir"
        write_bank(bank_path, make_demo_bank())
        summary = {
            "backend": "cpu",
            "sample_rate": 48000,
            "channels": 2,
            "frames": len(samples) // 2,
            "synthetic": True,
            "description": "Original test tones and synthetic gain/delay filters verify the software pipeline; these are not measured HRIR or a validated spatial-hearing demonstration.",
            "cases": [],
        }
        for name, direction, source in (("direction_a", 0, "input.pcm"), ("direction_b", 1, "input.pcm"), ("silence", 0, "silence_input.pcm")):
            output_pcm = output_dir / (name + ".pcm")
            stats = output_dir / (name + ".csv")
            command = [str(binary), "--input", str(output_dir / source), "--output", str(output_pcm),
                       "--bank", str(bank_path), "--backend", "cpu", "--direction", str(direction),
                       "--sample-rate", "48000", "--stats", str(stats)]
            run = subprocess.run(command, capture_output=True, text=True)
            if run.returncode:
                raise ValueError("{} backend failed ({}): {}".format(name, run.returncode, run.stderr.strip() or run.stdout.strip()))
            pcm_to_wav(output_pcm, output_dir / (name + ".wav"))
            with stats.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            data = output_pcm.read_bytes()
            if len(data) != len(samples) * 2:
                raise ValueError("{} output frame count does not match input".format(name))
            if name == "silence" and any(data):
                raise ValueError("silence case produced nonzero PCM")
            summary["cases"].append({
                "name": name,
                "direction": direction,
                "pcm": output_pcm.name,
                "wav": name + ".wav",
                "stats": stats.name,
                "stats_rows": rows,
                "sha256": hashlib.sha256(data).hexdigest(),
                "stdout": run.stdout.strip(),
                "stderr": run.stderr.strip(),
            })
        (output_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print("Synthetic CPU demo results: {}".format(output_dir))
    except (OSError, ValueError) as error:
        parser.exit(2, "error: {}\n".format(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
