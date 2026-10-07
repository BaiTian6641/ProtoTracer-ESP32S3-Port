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
    def trace(self, early=False, unblanked=False):
        raw = bytearray()
        for row in range(6):
            burst = bytearray([4 | ((row & 3) << 6)] * 650)
            for pixel in range(128):
                for offset in range(2): burst[pixel * 5 + offset] |= 1 | 8
            latch = 633 if early else 640
            burst[latch:latch + 2] = bytes(v | 2 for v in burst[latch:latch + 2])
            for i in range(645, 648): burst[i] &= ~4
            if unblanked: burst[100] &= ~4
            raw.extend(burst)
        temp_root = Path(__file__).resolve().parents[1] / 'build/s31-analyzer-test'
        temp_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root) as temp:
            assert Path(temp).resolve().is_relative_to(temp_root.resolve())
            path = Path(temp) / "trace.bin"
            path.write_bytes(raw)
            path.with_suffix('.json').write_text(json.dumps({'sample_rate_hz':20000000}))
            with contextlib.redirect_stdout(io.StringIO()): return analyze(path)

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


if __name__ == '__main__': unittest.main()
