#!/usr/bin/env python3
"""Prepare 48 kHz PCM and causal Q15 HRIR banks without third-party packages."""

import argparse
from array import array
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import sys
import wave


SAMPLE_RATE = 48000
MAX_TAPS = 1024
BANK_MAGIC = "SPATIAL_HRIR_V1"


@dataclass(frozen=True)
class HRIRBank:
    sample_rate: int
    taps: int
    synthetic: bool
    coefficients: tuple


def round_half_away(value):
    return int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))


def saturate_int16(value):
    return max(-32768, min(32767, value))


def _decode_int16(data):
    if len(data) % 2:
        raise ValueError("16-bit PCM contains an incomplete sample")
    samples = array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()
    return list(samples)


def _encode_int16(samples):
    if any(not isinstance(v, int) or v < -32768 or v > 32767 for v in samples):
        raise ValueError("PCM samples must be signed 16-bit integers")
    result = array("h", samples)
    if sys.byteorder != "little":
        result.byteswap()
    return result.tobytes()


def _write_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def read_raw_pcm(path):
    """Read headerless S16_LE stereo PCM; every frame must contain L and R."""
    data = Path(path).read_bytes()
    if len(data) % 4:
        raise ValueError("stereo S16_LE PCM size must be a multiple of 4 bytes")
    return _decode_int16(data)


def write_raw_pcm(path, samples):
    if len(samples) % 2:
        raise ValueError("stereo PCM requires an even number of samples")
    _write_bytes(path, _encode_int16(samples))


def read_wav(path, expected_rate=SAMPLE_RATE, allowed_channels=(1, 2)):
    """Return flat int16 samples and channel count, rejecting implicit conversion."""
    with wave.open(str(path), "rb") as source:
        if source.getcomptype() != "NONE" or source.getsampwidth() != 2:
            raise ValueError("WAV must contain uncompressed signed 16-bit PCM")
        channels = source.getnchannels()
        if channels not in allowed_channels:
            raise ValueError("unsupported WAV channel count: {}".format(channels))
        rate = source.getframerate()
        if expected_rate is not None and rate != expected_rate:
            raise ValueError("WAV sample rate {} does not match {} Hz".format(rate, expected_rate))
        frames = source.getnframes()
        data = source.readframes(frames)
        if len(data) != frames * channels * 2:
            raise ValueError("WAV contains truncated PCM data")
    return _decode_int16(data), channels, rate


def write_wav(path, samples, sample_rate=SAMPLE_RATE):
    if sample_rate != SAMPLE_RATE:
        raise ValueError("project audio must use 48000 Hz")
    if len(samples) % 2:
        raise ValueError("stereo WAV requires an even number of samples")
    data = _encode_int16(samples)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(data)


def prepare_mono_stereo(samples, channels):
    """Duplicate mono, or average stereo toward zero then duplicate the result."""
    if channels == 1:
        return [value for sample in samples for value in (sample, sample)]
    if channels != 2 or len(samples) % 2:
        raise ValueError("input must contain complete mono or stereo frames")
    result = []
    for index in range(0, len(samples), 2):
        total = samples[index] + samples[index + 1]
        mono = total // 2 if total >= 0 else -((-total) // 2)
        result.extend((mono, mono))
    return result


def wav_to_pcm(input_path, output_path):
    samples, channels, _ = read_wav(input_path)
    write_raw_pcm(output_path, prepare_mono_stereo(samples, channels))


def pcm_to_wav(input_path, output_path):
    # Preserve separate rendered left/right channels on output.
    write_wav(output_path, read_raw_pcm(input_path))


def generate_test_samples(duration=0.5, frequency=440.0, amplitude=0.08, silence=False):
    """Generate an original low-level, gently faded test signal as dual mono."""
    if not math.isfinite(duration) or duration <= 0:
        raise ValueError("duration must be positive and finite")
    if not math.isfinite(frequency) or not 0 < frequency < SAMPLE_RATE / 3:
        raise ValueError("frequency must be between 0 and 16000 Hz")
    if not math.isfinite(amplitude) or not 0 <= amplitude <= 0.25:
        raise ValueError("amplitude must be between 0 and 0.25")
    frames = round_half_away(duration * SAMPLE_RATE)
    if frames < 1:
        raise ValueError("duration must contain at least one sample frame")
    fade_frames = min(480, max(1, frames // 2))
    result = []
    for index in range(frames):
        envelope = min(1.0, index / fade_frames, (frames - 1 - index) / fade_frames)
        phase = 2 * math.pi * frequency * index / SAMPLE_RATE
        value = 0 if silence else round_half_away(
            32767 * amplitude * envelope * (math.sin(phase) + 0.25 * math.sin(1.5 * phase)) / 1.25
        )
        result.extend((value, value))
    return result


def validate_bank(bank):
    if bank.sample_rate <= 0:
        raise ValueError("HRIR sample rate must be positive")
    if not 1 <= bank.taps <= MAX_TAPS:
        raise ValueError("HRIR length must be between 1 and 1024 taps; no truncation is performed")
    if len(bank.coefficients) != 4 or any(len(row) != bank.taps for row in bank.coefficients):
        raise ValueError("HRIR bank requires four equal-length coefficient arrays")
    for row in bank.coefficients:
        _encode_int16(row)


def read_bank(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if not lines:
        raise ValueError("empty HRIR bank")
    header = lines[0].split()
    if len(header) != 4 or header[0] != BANK_MAGIC or header[3] not in ("synthetic=0", "synthetic=1"):
        raise ValueError("invalid SPATIAL_HRIR_V1 header")
    try:
        rate, taps = int(header[1], 10), int(header[2], 10)
        values = [int(token, 10) for token in " ".join(lines[1:]).split()]
    except ValueError as error:
        raise ValueError("HRIR bank numbers must be decimal integers") from error
    if not 1 <= taps <= MAX_TAPS or len(values) != 4 * taps:
        raise ValueError("HRIR bank coefficient count does not match its tap count")
    rows = tuple(tuple(values[index * taps:(index + 1) * taps]) for index in range(4))
    bank = HRIRBank(rate, taps, header[3] == "synthetic=1", rows)
    validate_bank(bank)
    return bank


def write_bank(path, bank):
    validate_bank(bank)
    header = "{} {} {} synthetic={}\n".format(BANK_MAGIC, bank.sample_rate, bank.taps, int(bank.synthetic))
    body = "\n".join(" ".join(str(value) for value in row) for row in bank.coefficients) + "\n"
    _write_bytes(path, (header + body).encode("utf-8"))


def make_demo_bank():
    """Synthetic gain/delay bank for plumbing tests, not a measured HRIR dataset."""
    left, right = [0] * 512, [0] * 512
    left[0], left[24] = 19661, 3277
    right[12], right[36] = 13107, 1638
    return HRIRBank(SAMPLE_RATE, 512, True, (tuple(left), tuple(right), tuple(right), tuple(left)))


def resample_hrir(samples, input_rate, output_rate):
    """Offline bandlimited interpolation with a 32-lobe Blackman-windowed sinc.

    Time zero is retained, out-of-range input is zero, and output length is
    ceil(input_length * output_rate / input_rate). No source tail is truncated.
    These are discrete FIR coefficients, so interpolation is multiplied by
    input_rate/output_rate to preserve convolution gain, rather than waveform
    sample amplitude. The finite zero-extended duration can cause edge error.
    """
    if input_rate <= 0 or output_rate <= 0 or not samples:
        raise ValueError("resampling requires non-empty samples and positive sample rates")
    if input_rate == output_rate:
        return list(samples), 0
    cutoff = 0.94 * min(1.0, output_rate / input_rate)
    coefficient_gain = input_rate / output_rate
    radius = 32.0 / cutoff
    count = (len(samples) * output_rate + input_rate - 1) // input_rate
    output, clipped = [], 0
    for index in range(count):
        position = index * input_rate / output_rate
        first = math.ceil(position - radius)
        last = math.floor(position + radius)
        value, weight_sum = 0.0, 0.0
        for source_index in range(first, last + 1):
            distance = position - source_index
            unit = distance / radius
            window = 0.42 + 0.5 * math.cos(math.pi * unit) + 0.08 * math.cos(2 * math.pi * unit)
            sinc_arg = cutoff * distance
            sinc = 1.0 if abs(sinc_arg) < 1e-14 else math.sin(math.pi * sinc_arg) / (math.pi * sinc_arg)
            weight = cutoff * sinc * window
            weight_sum += weight
            if 0 <= source_index < len(samples):
                value += samples[source_index] * weight
        quantized = round_half_away(value / weight_sum * coefficient_gain)
        saturated = saturate_int16(quantized)
        clipped += int(saturated != quantized)
        output.append(saturated)
    return output, clipped


def import_hrir(input_paths, output_path, source, license_text, resample=False):
    """Import A-left/A-right/B-left/B-right WAVs in causal coefficient order."""
    if len(input_paths) != 4:
        raise ValueError("exactly four HRIR WAV files are required")
    if not source.strip() or not license_text.strip():
        raise ValueError("source and license descriptions are required")
    output_path = Path(output_path)
    protected_paths = {Path(path).resolve() for path in input_paths}
    if output_path.resolve() in protected_paths or Path(str(output_path) + ".json").resolve() in protected_paths:
        raise ValueError("HRIR output and metadata must not overwrite any source WAV")
    originals = []
    for path in input_paths:
        samples, _, rate = read_wav(path, expected_rate=None, allowed_channels=(1,))
        if not samples:
            raise ValueError("HRIR WAV must not be empty: {}".format(path))
        originals.append((samples, rate))
    if len(set(rate for _, rate in originals)) != 1:
        raise ValueError("all four HRIR WAV files must have the same original sample rate")
    input_rate = originals[0][1]
    if input_rate != SAMPLE_RATE and not resample:
        raise ValueError(
            "HRIR sample rate is {} Hz; project audio is 48000 Hz. "
            "Use --resample for explicit offline windowed-sinc conversion.".format(input_rate)
        )
    converted, inputs = [], []
    for path, (samples, rate) in zip(input_paths, originals):
        values, clipped = resample_hrir(samples, rate, SAMPLE_RATE)
        converted.append(values)
        inputs.append({
            "path": str(Path(path).resolve()),
            "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(),
            "original_sample_rate": rate,
            "original_taps": len(samples),
            "output_sample_rate": SAMPLE_RATE,
            "output_taps_before_padding": len(values),
            "saturated_samples": clipped,
        })
    taps = max(len(row) for row in converted)
    if taps > MAX_TAPS:
        raise ValueError("converted HRIR has {} taps, exceeding 1024; no data was truncated".format(taps))
    rows = tuple(tuple(row + [0] * (taps - len(row))) for row in converted)
    bank = HRIRBank(SAMPLE_RATE, taps, False, rows)
    write_bank(output_path, bank)
    metadata = {
        "schema": "SPATIAL_HRIR_METADATA_V1",
        "synthetic": False,
        "sample_rate": SAMPLE_RATE,
        "taps": taps,
        "coefficient_order": ["A_left", "A_right", "B_left", "B_right"],
        "coefficient_time_order": "causal; no reversal",
        "source": source,
        "license": license_text,
        "inputs": inputs,
        "conversion": {
            "method": "none" if input_rate == SAMPLE_RATE else "32-lobe Blackman-windowed sinc",
            "cutoff_fraction_of_input_nyquist": 1.0 if input_rate == SAMPLE_RATE else 0.94 * min(1.0, SAMPLE_RATE / input_rate),
            "time_origin": "preserved",
            "impulse_response_gain_scale": input_rate / SAMPLE_RATE,
            "boundary": "zero padded",
            "boundary_limit": "finite original duration is preserved; sinc ringing outside that duration is omitted",
            "length": "ceil(original_taps * output_rate / input_rate), then zero pad to common taps",
            "quantization": "nearest, half away from zero, saturate int16",
        },
        "bank_sha256": hashlib.sha256(output_path.read_bytes()).hexdigest(),
    }
    _write_bytes(str(output_path) + ".json", (json.dumps(metadata, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return metadata


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate = commands.add_parser("generate", help="generate original low-amplitude 48 kHz dual-mono audio")
    generate.add_argument("--output", required=True)
    generate.add_argument("--duration", type=float, default=0.5)
    generate.add_argument("--frequency", type=float, default=440.0)
    generate.add_argument("--amplitude", type=float, default=0.08)
    generate.add_argument("--silence", action="store_true")
    convert = commands.add_parser("convert", help="WAV to dual-mono PCM, or rendered stereo PCM to WAV")
    convert.add_argument("--input", required=True)
    convert.add_argument("--output", required=True)
    importer = commands.add_parser("import-hrir", help="import measured HRIR WAVs; source data is not bundled")
    for side in ("a-left", "a-right", "b-left", "b-right"):
        importer.add_argument("--" + side, required=True)
    importer.add_argument("--output", required=True)
    importer.add_argument("--source", required=True, help="dataset source description or URL")
    importer.add_argument("--license", required=True, help="license description or license-file path; no license file is copied")
    importer.add_argument("--resample", action="store_true")
    synthetic = commands.add_parser("make-demo-bank", help="write a synthetic gain/delay bank, not real HRIR")
    synthetic.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "generate":
            samples = generate_test_samples(args.duration, args.frequency, args.amplitude, args.silence)
            if Path(args.output).suffix.lower() == ".wav":
                write_wav(args.output, samples)
            else:
                write_raw_pcm(args.output, samples)
        elif args.command == "convert":
            if Path(args.input).suffix.lower() == ".wav" and Path(args.output).suffix.lower() != ".wav":
                wav_to_pcm(args.input, args.output)
            elif Path(args.input).suffix.lower() != ".wav" and Path(args.output).suffix.lower() == ".wav":
                pcm_to_wav(args.input, args.output)
            else:
                raise ValueError("convert expects WAV -> raw PCM or raw PCM -> WAV")
        elif args.command == "import-hrir":
            metadata = import_hrir(
                [args.a_left, args.a_right, args.b_left, args.b_right], args.output,
                args.source, args.license, args.resample,
            )
            clipped = sum(item["saturated_samples"] for item in metadata["inputs"])
            if clipped:
                print("warning: {} resampled coefficients saturated; inspect metadata".format(clipped), file=sys.stderr)
        else:
            write_bank(args.output, make_demo_bank())
    except (ValueError, OSError, wave.Error) as error:
        parser.exit(2, "error: {}\n".format(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
