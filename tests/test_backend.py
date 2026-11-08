"""Behavior checks for the compiled CLI; Linux-only FIFO checks run in CI."""
import csv
import io
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audio_tools import make_demo_bank, read_raw_pcm, write_bank, write_raw_pcm
from reference import convolve, q15_round_saturate

BINARY = ROOT / "build" / ("spatial-audio.exe" if os.name == "nt" else "spatial-audio")


@unittest.skipUnless(BINARY.is_file(), "build the C backend with tools/build.py first")
class BackendTests(unittest.TestCase):
    def setUp(self):
        # All generated files stay under the project workspace.
        (ROOT / "build").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="test-backend-", dir=str(ROOT / "build"))
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name)
        self.bank = make_demo_bank()
        self.bank_path = self.path / "test.hrir"
        write_bank(self.bank_path, self.bank)
        randomizer = random.Random(8168)
        self.samples = [randomizer.randint(-16000, 16000) for _ in range(2077 * 2)]
        self.source = self.path / "input.pcm"
        self.output = self.path / "output.pcm"
        write_raw_pcm(self.source, self.samples)

    def run_cli(self, *arguments, **kwargs):
        return subprocess.run([str(BINARY)] + list(map(str, arguments)), capture_output=True, timeout=15, **kwargs)

    def command(self, *extra):
        return [str(BINARY), "--input", str(self.source), "--output", str(self.output),
                "--bank", str(self.bank_path)] + list(map(str, extra))

    def test_a_and_b_match_independent_whole_signal_convolution(self):
        for direction in (0, 1):
            with self.subTest(direction=direction):
                run = subprocess.run(self.command("--direction", direction), capture_output=True, timeout=15)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(read_raw_pcm(self.output), convolve(self.samples, self.bank, direction))

    def test_binary_standard_streams_and_bypass_downmix(self):
        run = self.run_cli("--bypass", input=self.source.read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        actual = struct.unpack("<{}h".format(len(run.stdout) // 2), run.stdout)
        expected = []
        for i in range(0, len(self.samples), 2):
            total = self.samples[i] + self.samples[i + 1]
            mono = total // 2 if total >= 0 else -((-total) // 2)
            expected.extend((mono, mono))
        self.assertEqual(list(actual), expected)

    def test_partial_final_block_stats_and_no_filter_tail_flush(self):
        stats = self.path / "stats.csv"
        run = subprocess.run(self.command("--stats", stats), capture_output=True, timeout=15)
        self.assertEqual(run.returncode, 0, run.stderr)
        with stats.open(newline="") as handle:
            rows = list(csv.DictReader(handle))
        self.assertEqual([int(row["frames"]) for row in rows], [1024, 1024, 29])
        self.assertTrue(all(row["backend"] == "cpu" and row["synthetic"] == "1" for row in rows))
        self.assertEqual(self.output.stat().st_size, self.source.stat().st_size)

    def test_one_block_crossfade_matches_independent_two_filter_mix(self):
        source = self.samples + self.samples
        source = source[:3072 * 2]
        write_raw_pcm(self.source, source)
        run = subprocess.run(self.command("--switch-at-block", 1), capture_output=True, timeout=15)
        self.assertEqual(run.returncode, 0, run.stderr)
        left = convolve(source, self.bank, 0)
        right = convolve(source, self.bank, 1)
        expected = left[:2048]
        for frame in range(1024, 2048):
            alpha = (frame - 1024) * 32768 // 1023
            for ear in (0, 1):
                index = frame * 2 + ear
                expected.append(q15_round_saturate((32768 - alpha) * left[index] + alpha * right[index]))
        expected += right[4096:]
        self.assertEqual(read_raw_pcm(self.output), expected)

    def test_empty_and_muted_streams(self):
        run = self.run_cli(input=b"")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, b"")
        run = self.run_cli("--mute", input=self.source.read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, bytes(self.source.stat().st_size))

    def test_truncated_stereo_frame_is_rejected(self):
        run = self.run_cli(input=b"\0\0\0")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn(b"Truncated PCM", run.stderr)

    def test_rate_mismatch_rejected_before_creating_output(self):
        run = subprocess.run(self.command("--sample-rate", 44100), capture_output=True, timeout=15)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn(b"Sample rate mismatch", run.stderr)
        self.assertFalse(self.output.exists())

    def test_bank_rejects_extra_missing_out_of_range_and_huge_numbers(self):
        valid = self.bank_path.read_text()
        header, coefficients = valid.split("\n", 1)
        tokens = coefficients.split()
        cases = [valid + "extra", header + "\n" + " ".join(tokens[:-1]),
                 header + "\n32768 " + " ".join(tokens[1:]),
                 header + "\n" + "9" * 300 + " " + " ".join(tokens[1:]),
                 header.replace("synthetic=1", "synthetic=2") + "\n" + coefficients]
        for text in cases:
            with self.subTest(case=text[:60]):
                self.bank_path.write_text(text)
                run = subprocess.run(self.command(), capture_output=True, timeout=15)
                self.assertNotEqual(run.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_input_alias_is_not_truncated(self):
        before = self.source.read_bytes()
        run = self.run_cli("--input", self.source, "--output", self.source)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(self.source.read_bytes(), before)
        run = self.run_cli("--input", self.source, "--stats", self.source)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(self.source.read_bytes(), before)

    def test_rpe_unavailable_is_explicit_without_cpu_fallback(self):
        run = subprocess.run(self.command("--backend", "rpe"), capture_output=True, timeout=15)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn(b"rpe", run.stderr.lower())
        self.assertFalse(self.output.exists())

    def test_invalid_cli_arguments(self):
        for args in [("--direction", "2"), ("--max-blocks", "0"), ("--fade-frames", "-1"),
                     ("--sample-rate", "0"), ("--unknown", "1"), ("--input",)]:
            with self.subTest(args=args):
                self.assertNotEqual(self.run_cli(*args).returncode, 0)

    @unittest.skipIf(os.name == "nt", "Linux FIFO behavior")
    def test_fifo_short_writes_are_reassembled_without_data_loss(self):
        fifo = self.path / "audio.fifo"
        os.mkfifo(str(fifo), 0o600)
        process = subprocess.Popen(self.command("--input", fifo), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop_process, process)
        deadline = time.monotonic() + 5
        while True:
            try:
                fd = os.open(str(fifo), os.O_WRONLY | os.O_NONBLOCK)
                os.set_blocking(fd, True)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() > deadline:
                    _, error = process.communicate(timeout=2)
                    self.fail("backend did not open audio FIFO: " + error.decode(errors="replace"))
                time.sleep(0.01)
        try:
            data = self.source.read_bytes()
            for i in range(0, len(data), 13):
                os.write(fd, data[i:i + 13])
        finally:
            os.close(fd)
        _, stderr = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, stderr)
        self.assertEqual(read_raw_pcm(self.output), convolve(self.samples, self.bank))

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()

    def await_path(self, path, process):
        deadline = time.monotonic() + 5
        while not path.exists():
            if process.poll() is not None:
                _, error = process.communicate()
                self.fail("backend exited before creating FIFO: " + error.decode(errors="replace"))
            if time.monotonic() > deadline:
                self.fail("backend did not create FIFO")
            time.sleep(0.01)

    @unittest.skipIf(os.name == "nt", "Linux FIFO EOF")
    def test_empty_named_fifo_terminates_after_writer_closes(self):
        fifo = self.path / "empty.fifo"
        os.mkfifo(str(fifo), 0o600)
        process = subprocess.Popen(self.command("--input", fifo), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop_process, process)
        deadline = time.monotonic() + 5
        while True:
            try:
                fd = os.open(str(fifo), os.O_WRONLY | os.O_NONBLOCK)
                os.close(fd)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() > deadline:
                    self.fail("backend did not open the empty input FIFO")
                time.sleep(0.01)
        _, error = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, error)
        self.assertEqual(self.output.read_bytes(), b"")

    @unittest.skipIf(os.name == "nt", "Linux FIFO behavior")
    def test_control_is_nonblocking_and_mute_direction_bypass_reach_csv(self):
        write_raw_pcm(self.source, self.samples * 30)
        runtime = self.path / "runtime"
        stats = self.path / "stats.csv"
        process = subprocess.Popen(self.command("--runtime-dir", runtime, "--stats", stats, "--pace"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop_process, process)
        self.await_path(runtime / "control.fifo", process)
        fd = os.open(str(runtime / "control.fifo"), os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(fd, b"STOP\nDIRECTION 1\nBYPASS 1\n")
            time.sleep(0.15)
            os.write(fd, b"START\n")
        finally:
            os.close(fd)
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, error)
        with stats.open(newline="") as handle:
            rows = list(csv.DictReader(handle))
        self.assertTrue(any(row["running"] == "0" for row in rows))
        self.assertTrue(any(row["running"] == "1" and row["direction"] == "1" and row["bypass"] == "1" for row in rows))
        self.assertIn(b"visual_drops=", error)  # No telemetry reader ever opened.

    @unittest.skipIf(os.name == "nt", "Linux FIFO behavior")
    def test_regular_file_cannot_masquerade_as_control_fifo(self):
        runtime = self.path / "runtime"
        runtime.mkdir(mode=0o700)
        control = runtime / "control.fifo"
        control.write_text("keep me")
        run = subprocess.run(self.command("--runtime-dir", runtime), capture_output=True, timeout=15)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(control.read_text(), "keep me")

    @unittest.skipIf(os.name == "nt", "Linux FIFO backpressure")
    def test_stalled_preview_reader_does_not_stall_audio(self):
        import fcntl
        write_raw_pcm(self.source, self.samples * 30)
        runtime = self.path / "runtime"
        process = subprocess.Popen(self.command("--runtime-dir", runtime, "--pace"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop_process, process)
        self.await_path(runtime / "telemetry.fifo", process)
        reader = os.open(str(runtime / "telemetry.fifo"), os.O_RDONLY | os.O_NONBLOCK)
        try:
            fcntl.fcntl(reader, fcntl.F_SETPIPE_SZ, 4096)
            # Deliberately never drain previews; the main audio stream must end.
            _, error = process.communicate(timeout=15)
        finally:
            os.close(reader)
        self.assertEqual(process.returncode, 0, error)
        self.assertEqual(self.output.stat().st_size, self.source.stat().st_size)
        drops = int(error.rsplit(b"visual_drops=", 1)[1].split()[0])
        self.assertGreater(drops, 1)

    @unittest.skipIf(os.name == "nt", "POSIX plugin loader")
    def test_plugin_abi_uses_copy_fixture_not_a_hardware_claim(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("C compiler needed to build isolated test plugin")
        fixture = self.path / "fixture.c"
        fixture.write_text(r'''
#include "spatial_rpe_plugin.h"
#include <stdlib.h>
#include <string.h>
static void *start(const spatial_bank *b, size_t f, spatial_mode m, char *e, size_t n)
{ (void)b; (void)f; (void)m; (void)e; (void)n; return malloc(1); }
static int mode(void *p, spatial_mode m, char *e, size_t n)
{ (void)p; (void)m; (void)e; (void)n; return 0; }
static int copy(void *p,const int16_t *i,int16_t *o,size_t f,char *e,size_t n)
{ (void)p; (void)e; (void)n; memcpy(o,i,4*f); return 0; }
static const spatial_rpe_api api = {1,"test-only-copy-fixture",start,mode,copy,free};
const spatial_rpe_api *spatial_rpe_get_api(void) { return &api; }
''')
        library = self.path / "test-fixture.so"
        subprocess.run([cc, "-std=c99", "-shared", "-fPIC", "-I", str(ROOT / "include"),
                        str(fixture), "-o", str(library)], check=True, capture_output=True)
        run = subprocess.run(self.command("--backend", "rpe", "--rpe-plugin", library), capture_output=True, timeout=15)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.output.read_bytes(), self.source.read_bytes())
        self.output.unlink()
        fixture.write_text("const void *spatial_rpe_get_api(void) { return 0; }\n")
        subprocess.run([cc, "-shared", "-fPIC", str(fixture), "-o", str(library)], check=True, capture_output=True)
        run = subprocess.run(self.command("--backend", "rpe", "--rpe-plugin", library), capture_output=True, timeout=15)
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
