#!/usr/bin/env python3
"""Independent whole-signal Q15 binaural convolution oracle, without tail flush."""

import argparse
from pathlib import Path
import sys

if __package__:
    from .audio_tools import SAMPLE_RATE, read_bank, read_raw_pcm, write_raw_pcm
else:
    from audio_tools import SAMPLE_RATE, read_bank, read_raw_pcm, write_raw_pcm


def q15_round_saturate(accumulator):
    """Round signed Q30 to Q15, half away from zero, then saturate int16."""
    if accumulator >= 0:
        rounded = (accumulator + 16384) // 32768
    else:
        rounded = -((-accumulator + 16384) // 32768)
    return max(-32768, min(32767, rounded))


def convolve(samples, bank, direction=0):
    """Render a single mono source from stereo's toward-zero channel average.

    Python integers provide an unbounded accumulator independent of the C core.
    This reference directly indexes the entire input, with no streaming state.
    """
    if direction not in (0, 1):
        raise ValueError("direction must be 0 or 1")
    if bank.sample_rate != SAMPLE_RATE:
        raise ValueError("HRIR bank must use 48000 Hz")
    if len(samples) % 2:
        raise ValueError("input must contain complete stereo frames")
    mono = []
    for index in range(0, len(samples), 2):
        total = samples[index] + samples[index + 1]
        mono.append(total // 2 if total >= 0 else -((-total) // 2))
    left, right = bank.coefficients[2 * direction:2 * direction + 2]
    result = []
    for index in range(len(mono)):
        left_sum, right_sum = 0, 0
        for tap in range(min(bank.taps, index + 1)):
            value = mono[index - tap]
            left_sum += value * left[tap]
            right_sum += value * right[tap]
        result.extend((q15_round_saturate(left_sum), q15_round_saturate(right_sum)))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="48 kHz stereo S16_LE raw PCM")
    parser.add_argument("--output", required=True, help="reference stereo S16_LE raw PCM")
    parser.add_argument("--bank", required=True)
    parser.add_argument("--direction", type=int, choices=(0, 1), default=0)
    args = parser.parse_args(argv)
    try:
        if Path(args.input).resolve() == Path(args.output).resolve():
            raise ValueError("input and output must be different files")
        result = convolve(read_raw_pcm(args.input), read_bank(args.bank), args.direction)
        write_raw_pcm(args.output, result)
    except (ValueError, OSError) as error:
        parser.exit(2, "error: {}\n".format(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
