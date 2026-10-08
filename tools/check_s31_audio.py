"""Check live S31 microphone snapshots without transcribing or playing audio."""
import argparse
import json
from pathlib import Path
import serial
import numpy as np
from serial_debug import receive


def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    with serial.Serial(port=None, baudrate=115200, timeout=.3, write_timeout=2) as port:
        port.dtr = True; port.rts = False; port.port = args.port; port.open()
        receive(port, 'dbg cancel')
        _, before = receive(port, 'dbg stats')
        frames = []
        hashes = set()
        for _ in range(args.frames):
            metadata, data = receive(port, 'dbg dump samples')
            samples = np.frombuffer(data, dtype='<f4').copy()
            assert len(samples) == 256 and np.isfinite(samples).all()
            assert np.all((samples >= 0) & (samples <= 4095))
            hashes.add(metadata['fnv']); frames.append(samples)
        for target in ['fft', 'filtered']:
            _, data = receive(port, f'dbg dump {target}')
            assert np.isfinite(np.frombuffer(data, dtype='<f4')).all(), f'{target} has non-finite values'
        _, after = receive(port, 'dbg stats')
        _, integrity = receive(port, 'dbg check')
    array = np.stack(frames)
    array.astype('<f4').tofile(args.output / 'samples.f32')
    summary = dict(frames=args.frames, distinct_frames=len(hashes),
                   minimum=float(array.min()), maximum=float(array.max()),
                   mean=float(array.mean()), ac_rms_median=float(np.median(array.std(axis=1))),
                   clipped_samples=int(np.count_nonzero((array == 0) | (array == 4095))),
                   before=before, after=after, heap=integrity)
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    assert len(hashes) > 1 and summary['ac_rms_median'] > 0, 'No live microphone activity'
    assert any('heap=ok' in line for line in integrity)
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM6')
    parser.add_argument('--frames', type=int, default=16)
    parser.add_argument('--output', type=Path, default=Path('build/s31-audio-check'))
    args = parser.parse_args()
    if not 2 <= args.frames <= 128: parser.error('frames must be between 2 and 128')
    run(args)
