"""Known electrical traces validate the analyzer independently of the driver."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_s31_hub75 import analyze


class Hub75TraceTest(unittest.TestCase):
    def trace(self, early=False, unblanked=False, address_shift=0, rows=6, planes=1):
        raw = bytearray()
        for row in range(rows):
            burst = bytearray([4 | (((row >> address_shift) & 3) << 6)] * 650)
            for pixel in range(128):
                for offset in range(2): burst[pixel * 5 + offset] |= 1 | 8
            latch = 633 if early else 640
            burst[latch:latch + 2] = bytes(v | 2 for v in burst[latch:latch + 2])
            for i in range(645, 648): burst[i] &= ~4
            if unblanked: burst[100] &= ~4
            raw.extend(burst * planes)
        temp_root = Path(__file__).resolve().parents[1] / 'build/s31-analyzer-test'
        temp_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root) as temp:
            assert Path(temp).resolve().is_relative_to(temp_root.resolve())
            path = Path(temp) / "trace.bin"
            path.write_bytes(raw)
            path.with_suffix('.json').write_text(json.dumps({'sample_rate_hz':20000000}))
            with contextlib.redirect_stdout(io.StringIO()): return analyze(path, address_shift)

    def test_complete_rows_and_safe_latch(self):
        result = self.trace()
        self.assertEqual(result['clock_hz'], 4000000)
        self.assertEqual(result['complete_clock_bursts'], {'128':4})
        self.assertEqual(result['latches_per_complete_burst'], [1])
        self.assertEqual(result['latch_after_final_clock_ns']['min'], 250)
        self.assertEqual(result['oe_enabled_clock_edges'], 0)

    def test_latch_before_final_clock(self):
        self.assertLess(self.trace(early=True)['latch_after_final_clock_ns']['min'], 0)

    def test_unblanked_clock_is_reported(self):
        self.assertEqual(self.trace(unblanked=True)['oe_enabled_clock_edges'], 6)

    def test_high_address_bits_and_refresh(self):
        result = self.trace(address_shift=2, rows=32, planes=8)
        self.assertEqual(result['address_signals'], ['C','D'])
        self.assertTrue(result['cd_progression_mod4'])
        self.assertEqual(result['complete_cd_latch_runs'], [32])
        self.assertAlmostEqual(result['inferred_scan_refresh_hz'], 20000000 / (650 * 8 * 16))


if __name__ == '__main__': unittest.main()
