"""Radiometry against the vendor's native library.

tests/data/radiometry holds this camera's calibration packages, 24 recorded
frames (20 with the shutter closed, then 4 open) and, for the open frames, the
Y16 image and the temperature of every pixel computed by the vendor's
libguide_sdk_unitrend.so (Thermal Mobile 3.1.2) when fed the same frames:
low range with emissivity 0.95 at 0.6 m, high range with emissivity 0.90 at 1.0 m.
"""

import gzip
import json
from pathlib import Path

import numpy as np
import pytest

from uti120 import vendor_temp, vendor_y16
from uti120.frame import FRAME_BYTES
from uti120.radiometry import CameraCalibration, Radiometer, Settings, band_correction

DATA = Path(__file__).parent / "data" / "radiometry"
CASES = {"low": (False, 0.95, 0.6), "high": (True, 0.90, 1.0)}


def read(name):
    with gzip.open(DATA / f"{name}.gz") as f:
        return f.read()


@pytest.fixture(scope="module")
def frames():
    raw = read("frames.raw")
    return [raw[i:i + FRAME_BYTES] for i in range(0, len(raw), FRAME_BYTES)]


@pytest.mark.parametrize("case", CASES)
def test_matches_vendor_core(frames, case):
    high, emissivity, distance = CASES[case]
    package = read("high.bin" if high else "low.bin")
    y16_model = vendor_y16.Y16Model(vendor_y16.Package.parse(package))
    temp_model = vendor_temp.TemperatureModel(
        vendor_temp.Package.parse(package), vendor_temp.Settings(emissivity, 23.0, distance), high=high)
    state = vendor_temp.State()
    want_y16 = np.frombuffer(read(f"y16-{case}.i16"), "<i2").reshape(-1, 120, 90)
    want_t = np.frombuffer(read(f"temp-{case}.f32"), "<f4").reshape(-1, 120, 90)
    k = 0
    for frame in frames:
        y16 = y16_model.process(frame)
        state.update(np.frombuffer(frame, "<i2", 16))
        if np.frombuffer(frame, "<i2", 16)[12] == 1:
            continue
        assert np.array_equal(y16, want_y16[k])
        values, index = np.unique(y16, return_inverse=True)
        t = np.array([temp_model.temperature(int(v), state) for v in values], np.float32)
        assert np.array_equal(t[index].reshape(y16.shape), want_t[k])
        k += 1
    assert k == len(want_t) == 4


def test_y16_matches_vendor_core_on_synthetic_frames():
    """Branches real frames never reach, against the vendor core's own Y16.

    y16-synth.raw: 7 frames built from the recorded ones (2 shutter, 5 open) with
    column and row offsets beyond the stripe clamps, noise spanning the stripe
    weight thresholds, steep edges, saturated pixels, and FPA values on and
    between the focus knots so the K gear changes between frames.
    y16-synth-k.bin: low.bin with distinct gains per gear and bad pixels
    (corners, edges, isolated, clusters, a 5x5 block).  y16-synth.i16: Y16 of
    every frame from libguide_sdk_unitrend.so fed these two files.
    """
    raw = read("y16-synth.raw")
    want = np.frombuffer(read("y16-synth.i16"), "<i2").reshape(-1, 120, 90)
    model = vendor_y16.Y16Model(vendor_y16.Package.parse(read("y16-synth-k.bin")))
    frames = [raw[i:i + FRAME_BYTES] for i in range(0, len(raw), FRAME_BYTES)]
    assert len(frames) == len(want) == 7
    for k, frame in enumerate(frames):
        assert np.array_equal(model.process(frame), want[k]), f"frame {k}"


def test_radiometer_orientation_and_range(frames):
    cal = CameraCalibration(read("low.bin"), read("high.bin"), [10000, 0] * 4)  # identity correction
    r = Radiometer(cal, Settings(emissivity=0.95, distance=0.6))
    temps = [r.temperatures() for f in frames if r.feed(f)]
    want = np.frombuffer(read("temp-low.f32"), "<f4").reshape(-1, 120, 90)
    assert len(temps) == 4 and temps[0].shape == (90, 120)
    # Sensor orientation: the vendor's portrait image turned back clockwise.
    assert np.array_equal(temps[-1], np.rot90(want[-1], -1))
    # A running motherboard: everything between room temperature and ~70 C.
    assert 15 < temps[-1].min() < temps[-1].max() < 80


def test_band_correction():
    coeff = [9799, -5252, 10751, -9489, 10417, 18752, 10428, 22419]  # this camera
    t = np.array([-10.0, 30.0, 100.0, 120.0, 200.0], np.float32)
    got = band_correction(t, coeff, high_range=False)
    want = [0.9799 * -10 - 0.5252, 1.0751 * 30 - 0.9489, 1.0417 * 100 + 1.8752,
            1.0417 * 120 + 1.8752, 200.0]
    assert np.allclose(got, want, atol=1e-4)
    got_high = band_correction(t, coeff, high_range=True)
    assert np.isclose(got_high[3], 1.0428 * 120 + 2.2419, atol=1e-4)  # > 100 C in high range
    assert np.isclose(got_high[4], 1.0428 * 200 + 2.2419, atol=1e-4)


def sweep_cases():
    return json.loads(read("temp-sweep.json"))


@pytest.mark.parametrize("n", range(10))
def test_temperature_sweep_matches_vendor_core(n):
    """Y16 -> temperature over synthetic frame headers (FPA below the first knot,
    between knots, on a knot and near the last one; lens and shutter drifting
    after every shutter transition), both ranges, emissivity 0.5..1.0, distances
    below, between and above the calibration distances, reflected 10..60 C.
    The golden temperatures come from the vendor core (temp-sweep-*.f32.gz)."""
    case = sweep_cases()[n]
    package = read("high.bin" if case["mode"] else "low.bin")
    model = vendor_temp.TemperatureModel(
        vendor_temp.Package.parse(package),
        vendor_temp.Settings(case["emissivity"], case["reflected"], case["distance"]),
        high=bool(case["mode"]))
    with gzip.open(DATA / case["temps_file"]) as f:
        want = np.frombuffer(f.read(), "<f4").reshape(-1, len(case["y16"]))
    state = vendor_temp.State()
    rows = []
    for words in case["headers"]:
        state.update(np.array(words, np.int16))
        if words[12] == 0:
            rows.append([model.temperature(y, state) for y in case["y16"]])
    assert np.array_equal(np.array(rows, np.float32), want)
