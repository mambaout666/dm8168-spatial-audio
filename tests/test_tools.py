"""Behavioral checks for portable audio preparation and an independent FIR oracle."""

import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest
import wave

from tools import audio_tools as audio
from tools import reference


class ToolsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=str(Path(__file__).resolve().parent))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def wav(self, name, samples, rate=48000, channels=1, width=2):
        path = self.root / name
        with wave.open(str(path), "wb") as output:
            output.setnchannels(channels)
            output.setsampwidth(width)
            output.setframerate(rate)
            output.writeframes(struct.pack("<" + "h" * len(samples), *samples) if width == 2 else bytes(samples))
        return path

    def test_mono_preparation_and_rendered_stereo_wav_preservation(self):
        source = self.wav("mono.wav", [1, -2, 3000])
        pcm = self.root / "input.pcm"
        audio.wav_to_pcm(source, pcm)
        self.assertEqual(audio.read_raw_pcm(pcm), [1, 1, -2, -2, 3000, 3000])
        audio.write_raw_pcm(pcm, [100, -200, -300, 400])
        output = self.root / "rendered.wav"
        audio.pcm_to_wav(pcm, output)
        samples, channels, rate = audio.read_wav(output)
        self.assertEqual((samples, channels, rate), ([100, -200, -300, 400], 2, 48000))

    def test_stereo_downmix_matches_c_toward_zero(self):
        self.assertEqual(audio.prepare_mono_stereo([-2, 1, 2, -1, -32768, 32767], 2), [0, 0, 0, 0, 0, 0])

    def test_audio_format_and_rate_rejection(self):
        for name, kwargs in (("wrong_rate.wav", {"rate": 44100}), ("eight_bit.wav", {"width": 1})):
            with self.subTest(name=name), self.assertRaises(ValueError):
                audio.wav_to_pcm(self.wav(name, [1, 2], **kwargs), self.root / "out.pcm")
        malformed = self.root / "malformed.pcm"
        malformed.write_bytes(b"\x01\x00")
        with self.assertRaises(ValueError):
            audio.read_raw_pcm(malformed)

    def test_bank_roundtrip_and_strict_count(self):
        path = self.root / "synthetic.hrir"
        bank = audio.make_demo_bank()
        audio.write_bank(path, bank)
        self.assertEqual(audio.read_bank(path), bank)
        self.assertEqual(bank.coefficients[0][0], 19661)
        self.assertEqual(bank.coefficients[1][12], 13107)
        path.write_text("SPATIAL_HRIR_V1 48000 2 synthetic=1\n1 2 3\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            audio.read_bank(path)

    def test_hrir_import_preserves_causal_order_tail_and_provenance(self):
        paths = [self.wav("{}.wav".format(i), [i + 1] + [0] * i + [100 + i]) for i in range(4)]
        out = self.root / "bank.hrir"
        metadata = audio.import_hrir(paths, out, "Original test fixture", "CC0 test fixture")
        bank = audio.read_bank(out)
        self.assertFalse(bank.synthetic)
        self.assertEqual(bank.taps, 5)
        for index in range(4):
            self.assertEqual(bank.coefficients[index][0], index + 1)
            self.assertEqual(bank.coefficients[index][index + 1], 100 + index)
            self.assertEqual(metadata["inputs"][index]["sha256"], hashlib.sha256(paths[index].read_bytes()).hexdigest())
        self.assertEqual(json.loads(Path(str(out) + ".json").read_text(encoding="utf-8"))["taps"], 5)

    def test_hrir_rate_mismatch_requires_explicit_resampling(self):
        paths = [self.wav("{}.wav".format(i), [1000, 0, 0, 500], rate=44100) for i in range(4)]
        out = self.root / "bank.hrir"
        with self.assertRaisesRegex(ValueError, "--resample"):
            audio.import_hrir(paths, out, "test", "test")
        self.assertFalse(out.exists())
        metadata = audio.import_hrir(paths, out, "test", "test", resample=True)
        self.assertEqual(audio.read_bank(out).taps, 5)
        self.assertEqual(metadata["inputs"][0]["original_sample_rate"], 44100)
        self.assertIn("sinc", metadata["conversion"]["method"])

    def test_import_rejects_stereo_mixed_rates_and_oversized_filters(self):
        paths = [self.wav("base{}.wav".format(i), [1, 2]) for i in range(4)]
        stereo = self.wav("stereo.wav", [1, 2], channels=2)
        mismatch = self.wav("mismatch.wav", [1, 2], rate=44100)
        oversized = self.wav("long.wav", [0] * 1025)
        for replacement, allow_resample in ((stereo, False), (mismatch, True), (oversized, False)):
            with self.subTest(path=replacement.name), self.assertRaises(ValueError):
                audio.import_hrir([replacement] + paths[1:], self.root / "bad.hrir", "test", "test", allow_resample)

    def test_resampler_retains_duration_and_passband(self):
        samples = [1000] * 441
        output, clipped = audio.resample_hrir(samples, 44100, 48000)
        self.assertEqual(len(output), 480)
        self.assertEqual(clipped, 0)
        expected = 1000 * 44100 / 48000
        self.assertTrue(all(abs(value - expected) <= 1 for value in output[50:-50]))

    def test_resampler_rejects_out_of_band_aliasing(self):
        peaks = []
        for frequency in (1000, 14000):
            samples = [round(5000 * math.sin(2 * math.pi * frequency * index / 48000)) for index in range(960)]
            output, _ = audio.resample_hrir(samples, 48000, 24000)
            peaks.append(max(abs(value) for value in output[60:-60]))
        self.assertGreater(peaks[0], 9800)
        self.assertLess(peaks[1], 10)

    def test_hrir_resampling_preserves_filter_gain_and_physical_delay(self):
        samples = [0] * 441
        samples[100] = 10000
        output, clipped = audio.resample_hrir(samples, 44100, 48000)
        self.assertEqual(clipped, 0)
        self.assertLessEqual(abs(sum(output) - sum(samples)), 10)
        peak = max(range(len(output)), key=lambda index: abs(output[index]))
        self.assertLessEqual(abs(peak / 48000 - 100 / 44100), 1 / 48000)

    def test_resampling_never_silently_truncates_or_overwrites_source(self):
        paths = [self.wav("long{}.wav".format(i), [0] * 1000, rate=44100) for i in range(4)]
        output = self.root / "oversized.hrir"
        with self.assertRaisesRegex(ValueError, "no data was truncated"):
            audio.import_hrir(paths, output, "test", "test", resample=True)
        self.assertFalse(output.exists())
        original_hash = hashlib.sha256(paths[0].read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, "overwrite"):
            audio.import_hrir(paths, paths[0], "test", "test", resample=True)
        self.assertEqual(hashlib.sha256(paths[0].read_bytes()).hexdigest(), original_hash)

    def test_q15_half_rounding_and_saturation(self):
        self.assertEqual([reference.q15_round_saturate(x) for x in (16383, 16384, -16383, -16384)], [0, 1, 0, -1])
        self.assertEqual(reference.q15_round_saturate(32767 * 32768), 32767)
        self.assertEqual(reference.q15_round_saturate(32768 * 32768), 32767)
        self.assertEqual(reference.q15_round_saturate(-32769 * 32768), -32768)

    def test_reference_known_impulse_direction_and_no_tail_flush(self):
        bank = audio.HRIRBank(48000, 3, False, ((16384, 8192, 0), (0, 16384, 0), (0, 0, 16384), (16384, 0, 0)))
        samples = [16384, 16384, 0, 0, 0, 0, 0, 0]
        self.assertEqual(reference.convolve(samples, bank, 0), [8192, 0, 4096, 8192, 0, 0, 0, 0])
        self.assertEqual(reference.convolve(samples, bank, 1), [0, 8192, 0, 0, 8192, 0, 0, 0])
        self.assertEqual(len(reference.convolve(samples, bank)), len(samples))

    def test_reference_saturates_without_accumulator_wrap(self):
        bank = audio.HRIRBank(48000, 4, False, ((32767,) * 4,) * 4)
        self.assertEqual(reference.convolve([32767, 32767] * 4, bank)[-2:], [32767, 32767])

    def test_original_audio_is_low_amplitude_and_silence_exact(self):
        samples = audio.generate_test_samples(duration=0.02)
        self.assertEqual(len(samples), 1920)
        self.assertLessEqual(max(abs(v) for v in samples), 2622)
        self.assertEqual(samples[::2], samples[1::2])
        self.assertTrue(all(v == 0 for v in audio.generate_test_samples(duration=0.02, silence=True)))


if __name__ == "__main__":
    unittest.main()
