"""Y16 -> temperature, ported from the vendor core libguide_sdk_unitrend.so
(Thermal Mobile 3.1.2) and checked against it (check_temp.py).

Function names in the comments are the decompiled C++ functions.

Calibration package (read from the camera's flash; ParsePackage):
  bytes [0, 0xd8)   DataHeader
      0x41 u8   n_focus: number of FPA-temperature breakpoints
      0x42 i16  T_min of the curves (degC); 0x44 i16 T_max
      0x4b u8   n_dist: curves per breakpoint (one per calibration distance)
      0x50 u16  curve_len: points per curve, 0.1 degC apart starting at T_min
      0x52 u16  focus_bytes; 0x54 i32 curve_bytes; 0x58 i32 k_bytes
      0x74 u16[n_dist] calibration distances * 10 (m)
      0x92      sensor serial (ASCII)
  bytes [0xf6, ...) focus table i16[n_focus] (degC*100), then the curves
      u16[n_focus][n_dist][curve_len] (absolute Y16 for each temperature),
      then the per-pixel K table (not used for temperatures).

Per-frame state (updateMeasureParam, from frame header words 8..12):
  shutter-at-start, shutter, lens, FPA temperature (degC*100) and the shutter
  flag; on every closed->open shutter transition the shutter, lens and FPA
  temperatures are remembered ("last") for the lens drift correction.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

f32 = np.float32


def s16(v: int) -> int:
    """C conversion to short (wraps)."""
    return ((int(v) + 0x8000) & 0xFFFF) - 0x8000
HERE = Path(__file__).resolve().parent
# nEmissCurve: temperature*10 for each of 0x4000 radiance levels (static table in the library).
EMISS_CURVE = np.frombuffer((HERE / "data" / "emiss_curve.bin").read_bytes(), "<i2").astype(np.int32)


@dataclass
class Package:
    t_min: int
    t_max: int
    curve_len: int
    focus: np.ndarray       # float32 breakpoints (degC)
    distances: np.ndarray   # float32 (m)
    curves: np.ndarray      # int32 [n_focus][n_dist][curve_len]
    serial: str

    @classmethod
    def parse(cls, data: bytes) -> Package:
        h = data[:0xd8]
        n_focus, n_dist = h[0x41], h[0x4b]
        t_min, t_max = struct.unpack_from("<hh", h, 0x42)
        curve_len, focus_bytes = struct.unpack_from("<HH", h, 0x50)
        curve_bytes, _k_bytes = struct.unpack_from("<ii", h, 0x54)
        assert focus_bytes == 2 * n_focus and curve_bytes == 2 * n_focus * n_dist * curve_len
        focus = np.frombuffer(data, "<i2", n_focus, 0xf6).astype(np.float32) / f32(100.0)
        curves = np.frombuffer(data, "<u2", n_focus * n_dist * curve_len, 0xf6 + focus_bytes)
        dist = np.array(struct.unpack_from(f"<{n_dist}H", h, 0x74), np.float32) / f32(10.0)
        serial = h[0x92:0xa4].split(b"\0")[0].decode("ascii", "replace")
        return cls(t_min, t_max, curve_len, focus, dist,
                   curves.astype(np.int32).reshape(n_focus, n_dist, curve_len), serial)


@dataclass
class State:
    """MEASURE_PARAM fields fed by updateMeasureParam (all float32, degC)."""
    startup_shutter: float = 0.0
    shutter: float = 0.0
    lens: float = 0.0
    fpa: float = 0.0
    shutter_flag: int = 0
    last_shutter_flag: int = 0
    last_shutter: float = 0.0
    lastlast_shutter: float = 0.0
    last_lens: float = 0.0
    lastlast_lens: float = 0.0
    last_fpa: float = 0.0

    def update(self, header_words):
        """updateMeasureParam(short*): header words 8..12 of every frame."""
        w = [int(x) for x in header_words]
        self.startup_shutter = f32(w[8]) / f32(100)
        self.shutter = f32(w[9]) / f32(100)
        self.lens = f32(w[10]) / f32(100)
        self.fpa = f32(w[11]) / f32(100)
        self.fpa_raw = w[11]
        self.shutter_flag = w[12]
        if self.last_shutter_flag == 1 and self.shutter_flag == 0:
            self.lastlast_shutter = self.last_shutter
            self.last_shutter = self.shutter
            self.lastlast_lens = self.last_lens
            self.last_lens = self.lens
            self.last_fpa = self.fpa
        self.last_shutter_flag = self.shutter_flag


@dataclass
class Settings:
    emissivity: float = 0.95     # SetEmissivity -> HOST_PARAM+0x38
    reflect: float = 23.0        # SetReflectTemp -> HOST_PARAM+0x40 (package default 23)
    distance: float = 0.8        # SetDistance -> MEASURE_PARAM+0x98 (initMeasureParam: 0.8)


# initMeasureParam constants.
LENS_K = (f32(-200.0), f32(-300.0))   # MEASURE_PARAM+0x74 (param 1), +0x78 (param 0)
EMISS_MAX = f32(0.98)                 # DAT_00142cc8
DRIFT_MIN = 0.1                       # DAT_00142d30 (double)


def lens_drift(state: State, high: bool) -> f32:
    """LensDriftCorrectZX01C(drift, param): GetLowTemperature passes 0, which
    selects MEASURE_PARAM+0x78 (-300); GetHighTemperature passes 1 (+0x74, -200)."""
    if float(f32(state.last_fpa)) >= DRIFT_MIN and float(f32(state.last_lens)) >= DRIFT_MIN:
        return (f32(state.lens) - f32(state.last_lens)) * LENS_K[0 if high else 1]
    return f32(0.0)


def search(curve: np.ndarray, v: int) -> int:
    """Index part of GetSingleCurveTemperature: the first point above v."""
    n = len(curve)
    if v < curve[0]:
        return 0
    if curve[n - 1] < v:
        return n
    # Linear scan for the first point above v (curves are not strictly
    # monotonic at their ends, so a binary search would differ).
    above = np.nonzero(curve[1:] > v)[0]
    return int(above[0]) + 1 if len(above) else 0   # none found: falls through to 0


def single_curve(y16: int, curve: np.ndarray, pkg: Package, state: State, high: bool) -> float:
    """GetSingleCurveTemperature: temperature from one curve (param_4 = high range)."""
    corr = lens_drift(state, high)                            # MEASURE_PARAM+0xa0 is set
    y = s16(int(f32(s16(y16)) - corr))         # (short)(int)((float)y16 - corr)
    base = int(f32(state.shutter) * f32(10.0) - f32(10.0) * f32(pkg.t_min))
    offset = f32(curve[base]) if 0 < base < len(curve) else f32(0.0)
    v = int(f32(y) * f32(1.0) + offset)                       # factor MEASURE_PARAM+0x4c*0x5c = 1
    j = search(curve, v)
    frac = float(f32(j / 10.0))
    return float(f32(0.0 + float(f32(pkg.t_min)) + frac))     # offsets +0x50,+0x60 = 0


def y16_from_t(t10: int) -> int:
    """GetY16FromT: radiance level for temperature*10 (binary search in nEmissCurve)."""
    c = EMISS_CURVE
    if not (c[0] < t10 < c[0x3fff]):
        return 0x3fff if t10 >= c[0x3fff] else 0
    lo, hi = 0, 0x3fff
    while True:
        while True:
            mid = (hi + lo) >> 1
            if c[mid] < t10:
                break
            if c[mid] <= t10:
                return mid
            hi = mid - 1
            if hi < lo:
                return mid
        lo = mid + 1
        if lo > hi:
            return mid


def emiss_cor(t10: int, y16_reflect: int, e100: int) -> int:
    """EmissCor(short T*10, short Y16 of the reflected temperature, int emissivity*100)."""
    c = EMISS_CURVE
    if e100 < 1:
        return int(c[0x3fff]) & 0xFFFF
    if e100 > 99:
        return t10
    t = s16(t10)
    u = 0x3fff
    if c[0x3fff] > t and c[0] < t:
        lo, hi = 0, 0x3fff
        while True:
            u = (hi + lo) >> 1
            if c[u] < t:
                lo = u + 1
                if lo > hi:
                    break
                continue
            if c[u] <= t:
                break
            hi = u - 1
            if hi < lo:
                break
    elif not (c[0] < t):
        u = 0
    if e100 < 99:
        num = u * 100 - y16_reflect * (100 - e100)
        u = int(num / e100)                                   # C division truncates toward zero
    u = min(u, 0x3fff)
    u = max(u, 0)
    return int(c[u]) & 0xFFFF


class TemperatureModel:
    """guideCoreMeasureTempByY16 for one measure range (getCurve + GetLow/HighTemperature)."""

    def __init__(self, pkg: Package, settings: Settings | None = None, high: bool = False):
        self.pkg = pkg
        self.settings = settings or Settings()
        self.high = high

    def bracket(self, state: State):
        """getCurve: FPA breakpoint bracket.  Returns (index byte, lower, upper)."""
        focus_raw = np.round(self.pkg.focus * 100).astype(int)   # i16 table
        fpa = state.fpa_raw
        n = len(focus_raw)
        if fpa < focus_raw[0]:
            return 0, None, 0
        if focus_raw[n - 1] < fpa:
            return n, n - 1, None
        for i in range(n):
            upper = i + 1 if i + 1 < n else i
            if focus_raw[i] <= fpa <= focus_raw[upper]:
                return i + 1, i, upper
        return 1, 0, 0

    def temperature(self, y16: int, state: State) -> float:
        pkg, s = self.pkg, self.settings
        idx, lo, up = self.bracket(state)
        n = len(pkg.focus)
        T = lambda f, d: f32(single_curve(y16, pkg.curves[f, d], pkg, state, self.high))
        t_lo = (T(lo, 0), T(lo, 1)) if lo is not None else (f32(0), f32(0))
        t_up = (T(up, 0), T(up, 1)) if up is not None else (f32(0), f32(0))

        # FPA weights (GetLowTemperature): w_up, w_lo.
        if idx == 0:
            w_up, w_lo = f32(1.0), f32(0.0)
        elif idx == n:
            w_up, w_lo = f32(0.0), f32(1.0)
        else:
            a = pkg.focus[idx] - f32(state.fpa)
            b = f32(state.fpa) - pkg.focus[idx - 1]
            ssum = b + a
            w_up = f32(1.0) - a / ssum
            w_lo = f32(1.0) - b / ssum

        # Distance weights.
        dist = f32(s.distance)
        d0, d1 = pkg.distances[0], pkg.distances[1]
        if dist <= d0:
            w_d0, w_d1 = f32(1.0), f32(0.0)
        elif dist < d1:
            a = float(dist - d0)
            b = float(d1 - dist)
            ssum = float(f32(b + a))
            w_d0 = f32(1.0 - a / ssum)
            w_d1 = f32(1.0 - b / ssum)
        else:
            w_d0, w_d1 = f32(0.0), f32(1.0)

        t = (w_up * t_up[0] + w_lo * t_lo[0]) * w_d0 + (w_up * t_up[1] + w_lo * t_lo[1]) * w_d1

        # Emissivity (MEASURE_PARAM+0xa3 set, type +0xb0 = 1).
        emiss = f32(s.emissivity)
        if emiss <= EMISS_MAX:
            yr = y16_from_t(int(f32(s.reflect) * f32(10.0)))
            t10 = s16(int(t * f32(10.0)))
            t = f32(s16(emiss_cor(t10, yr, int(emiss * f32(100.0))))) * f32(0.1)
        return float(t)
