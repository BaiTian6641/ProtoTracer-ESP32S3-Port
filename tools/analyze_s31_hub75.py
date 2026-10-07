"""Analyze partial HUB75 wiring: D0 CLK, D1 LAT, D2 OE, D3-5 RGB1, D6-7 A/B."""
import argparse
import json
from pathlib import Path
import numpy as np
import hashlib


def analyze(path):
    metadata = json.loads(path.with_suffix('.json').read_text())
    rate = metadata['sample_rate_hz']
    raw = np.frombuffer(path.read_bytes(), dtype=np.uint8)
    clk = (raw & 1) != 0
    lat = (raw & 2) != 0
    oe = (raw & 4) != 0
    rises = np.flatnonzero(~clk[:-1] & clk[1:]) + 1
    latches = np.flatnonzero(~lat[:-1] & lat[1:]) + 1
    result = {'samples':len(raw), 'sample_rate_hz':rate, 'clock_rises':len(rises), 'latches':len(latches),
              'capture_sha256':hashlib.sha256(raw).hexdigest()}
    if len(rises) > 2:
        deltas = np.diff(rises)
        period = float(np.median(deltas))
        result['clock_hz'] = rate / period
        splits = np.flatnonzero(deltas > period * 1.6) + 1
        burst_lengths = np.diff(np.r_[0,splits,len(rises)])
        # First/last burst can be clipped by the acquisition window.
        values, counts = np.unique(burst_lengths[1:-1], return_counts=True)
        result['complete_clock_bursts'] = {str(v):int(c) for v,c in zip(values,counts)}
        result['oe_enabled_clock_edges'] = int(np.count_nonzero(~oe[rises]))
        result['lat_high_clock_edges'] = int(np.count_nonzero(lat[rises]))
        bounds = np.r_[0,splits,len(rises)]
        distances, latch_counts = [], []
        for i in range(1, len(bounds) - 2):
            first, end = bounds[i], bounds[i + 1]
            selected = latches[(latches >= rises[first]) & (latches < rises[end])]
            latch_counts.append(len(selected))
            if len(selected) == 1: distances.append(int(selected[0]) - int(rises[end - 1]))
        result['latches_per_complete_burst'] = sorted(set(latch_counts))
        if distances:
            result['latch_after_final_clock_ns'] = {'min':float(min(distances) * 1e9 / rate),
                                                    'median':float(np.median(distances) * 1e9 / rate)}
    if len(latches):
        result['oe_enabled_at_latch'] = int(np.count_nonzero(~oe[latches]))
        result['observed_ab'] = sorted(set(((raw[latches] >> 6) & 3).tolist()))
        addresses = ((raw[latches] >> 6) & 3).astype(np.int16)
        changes = np.flatnonzero(addresses[1:] != addresses[:-1]) + 1
        runs = addresses[np.r_[0, changes]]
        result['ab_progression_mod4'] = bool(len(runs) > 1 and np.all((np.diff(runs) & 3) == 1))
        result['complete_ab_latch_runs'] = sorted(set(np.diff(np.r_[0, changes, len(addresses)])[1:-1].tolist()))
        if len(changes) > 1:
            # Row-start spacing excludes the unequal BCM intervals within rows.
            result['inferred_scan_refresh_hz'] = rate / float(np.median(np.diff(latches[changes]))) / 16
    ab = (raw >> 6) & 3
    result['ab_changes_while_oe_enabled'] = int(np.count_nonzero((ab[1:] != ab[:-1]) & (~oe[1:] | ~oe[:-1])))
    result['rgb1_nonzero_samples'] = int(np.count_nonzero(raw & 0x38))
    print(json.dumps(result,indent=2))
    path.with_suffix('.analysis.json').write_text(json.dumps(result,indent=2))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    analyze(parser.parse_args().capture)
