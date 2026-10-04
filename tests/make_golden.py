#!/usr/bin/env python3
"""Write reference outputs of the Python image processing to tests/data/golden.

The C++ tests compare their results with these files, so both implementations
are held to the same numbers.  Regenerate after an intended change to the
processing:  python3 tests/make_golden.py
"""

import gzip
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))

from uti120 import palette
from uti120.frame import FRAME_BYTES, Frame
from uti120.process import ADC_MAX, AutoGain, Calibration, row_stripes

DATA = ROOT / "tests" / "data"
OUT = DATA / "golden"


def load(name):
    with gzip.open(DATA / name) as f:
        raw = f.read()
    return [Frame.parse(raw[i:i + FRAME_BYTES]) for i in range(0, len(raw), FRAME_BYTES)]


def write(name, array):
    with gzip.GzipFile(OUT / f"{name}.gz", "wb", mtime=0) as f:
        f.write(np.ascontiguousarray(array).tobytes())


def main():
    OUT.mkdir(exist_ok=True)
    closed, opened = load("closed16.bin.gz"), load("open4.bin.gz")

    for name in palette.NAMES:
        write(f"lut_{name}.u8", palette.lut(name))

    cal = Calibration.from_frames(closed)
    write("dark.f32", cal.dark.astype("<f4"))
    write("bad.u8", cal.bad.astype(np.uint8))

    # Signals of the open frames and their rendering through one AutoGain,
    # so the temporal smoothing is covered too.
    gain = AutoGain()
    for k, f in enumerate(opened):
        s = cal.apply(f.pixels)
        write(f"signal{k}.f32", s.astype("<f4"))
        write(f"ironbow{k}.u8", palette.colorize(gain(s), "ironbow"))

    # Bad-pixel replacement, including a fully masked 5x5 block (all-NaN
    # neighbourhoods fall back to the image median) and a dead pixel.
    px = opened[0].pixels.copy()
    px[30:35, 50:55] = ADC_MAX
    px[0, 0] = 0
    s = cal.apply(px)
    write("forced.f32", s.astype("<f4"))
    write("forced_row_stripes.f64", np.array([row_stripes(s)], dtype="<f8"))
    print(f"wrote {len(list(OUT.iterdir()))} files to {OUT}")


if __name__ == "__main__":
    main()
