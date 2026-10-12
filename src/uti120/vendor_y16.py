"""Raw UTi120Mobile frame -> Y16, as computed by the vendor's native core.

Reproduces what libguide_sdk_unitrend.so (Thermal Mobile 3.1.2, x86_64 build)
returns from guideCoreGetCurrentY16 after guideCoreConvertXToImage(frame, ...,
rotateType=3), with guideCoreUpdateB(frame) called after every shutter-closed
frame.  Addresses below are of that library as loaded by Ghidra (image base
0x100000); the decompiled C is in radiometry/ghidra/c/.

Calibration package (guideCoreParsePackage @ 0x1150c0), little-endian:
    0x000..0x0d8   header (copied to mLowDataHeader / mHighDataHeader)
        0x41  u8   number of FPA-temperature knots ("focus" table) = n
        0x4c  u16  width (120)        0x4e u16  height (90)
        0x52  u16  focus table length in bytes (2n)
        0x54  u32  curve table length in bytes
        0x58  u32  K table length in bytes = n * width * height * 2
    0x0f6          focus table: n x int16, FPA temperatures in 0.01 C
    0x0f6+focus    curve table (temperature curves, not used for Y16)
    ...+curve      K table: n gears x (height x width) uint16; bit 15 marks a
                   bad pixel, bits 0..14 are the per-pixel gain in 1/8192
Measure mode 0 (the default, guideCoreSetMeasureMode(0)) uses the low
package (type 0), mode 1 the high one.

State across frames
    B     last shutter-closed frame's active pixels (guideCoreUpdateB @ 0x114f10
          copies frame bytes 0x2d0.., i.e. after the header row and the two
          reference rows).  Zero until the first shutter frame.
    gear  index of the K table in use.  guideCoreInit (0x1146c0) starts with
          gear 0; after every frame updateK (0x114b30) picks the gear from the
          frame's FPA temperature (header word 11, updateMeasureParam @ 0x114980):
            fpa <  focus[0]          -> 0
            fpa >  focus[n-1]        -> n
            focus[i] <= fpa <= focus[i+1] (first such i) -> i + 1
          So frame k is processed with the gear chosen from frame k-1.

Per frame (CInfraredCore::InfraredImageProcess @ 0x1171f0, switches as set by
initCInfraredCore @ 0x114560 and guideCoreConvertXToImage @ 0x114c70):
 1. NUC: v = (X - B) * (K & 0x7fff) as int32 with X, B read as uint16;
    out = int16(trunc(v / 8192))  (C division toward zero).
 2. ReplaceBadPoint (0x117c70) for pixels with K bit 15 set: descending sort
    of the valid 8-neighbours (row-major scan, in place); median, or for an
    even count  a[n/2-1]/2 + a[n/2]/2  with C int16 division.  Applied twice
    (nuc_switch and rpbp_switch); idempotent.
 3. Temporal filter: switched off by guideCoreConvertXToImage (put_tff_switch(false)).
 4. RemoveVerStripe (0x11a200), window 9, weight threshold 3200, clamp 25:
      F, W = range-weighted smoothing over a 9x1 (horizontal) window with
             edge replication (FixedPoint_GrayFilter_16bit_RSN @ 0x119ba0):
             weight = T[min(|c - v|, 511)], T[d] = round(4096 * exp(-d^2 / (2*25^2)));
             F = trunc(sum(T*v) / sum(T)) (centre value if sum(T) == 0),
             W = sum(T) // count
      D = clamp(in - F, -25, 25)
      col[x] = clamp(trunc(mean of D[:, x] over rows with W >= 3200), -25, 25)
      out = int16(in - col[x])
 5. RemoveHorStripe (0x11a600): the same along rows: window 1x9 (vertical),
    threshold 3500, clamp 20, out = int16(in - row[y]).
 6. GaussianFilter_16bit (0x11aa80): 3x3 kernel [[46,343,46],[343,2536,343],[46,343,46]]
    (floor(4096 * exp(-d^2/0.5) / sum)), edge replication, out = int16(sum >> 12).
 7. Rotation (0x11c300) type 2: 120x90 -> 90 wide x 120 tall, sensor right edge on top.
 Y16 is the result (output_y16 copy at the end of step 7).
"""
import struct
from dataclasses import dataclass, field

import numpy as np

W, H = 120, 90
N = W * H


@dataclass
class Package:
    focus: np.ndarray      # int16[n], FPA temperature knots in 0.01 C
    k: np.ndarray          # uint16[n, H*W]
    header: bytes
    curve: bytes

    @classmethod
    def parse(cls, data: bytes) -> "Package":
        header = data[:0xd8]
        n = header[0x41]
        width, height = struct.unpack_from("<HH", header, 0x4c)
        focus_len = struct.unpack_from("<H", header, 0x52)[0]
        curve_len, k_len = struct.unpack_from("<II", header, 0x54)
        if (width, height) != (W, H) or focus_len != 2 * n or k_len != n * W * H * 2:
            raise ValueError("unexpected calibration package geometry")
        off = 0xf6
        focus = np.frombuffer(data, "<i2", n, off)
        off += focus_len
        curve = data[off:off + curve_len]
        off += curve_len
        k = np.frombuffer(data, "<u2", n * N, off).reshape(n, N)
        return cls(focus.copy(), k.copy(), header, curve)


def range_weight_table(std: int) -> np.ndarray:
    d = np.arange(512, dtype=np.float64)
    return np.floor(4096.0 * np.exp(-(d * d) / (2.0 * std * std)) + 0.5).astype(np.int64)


GAUSS3 = np.array([[46, 343, 46], [343, 2536, 343], [46, 343, 46]], dtype=np.int64)


def trunc_div(a, b):
    """C integer division (toward zero) for numpy int64 arrays."""
    q = np.abs(a) // np.abs(b)
    return np.where((a < 0) ^ (b < 0), -q, q)


def int16(a):
    return (np.asarray(a, dtype=np.int64) & 0xFFFF).astype(np.uint16).view(np.int16)


def replace_bad_points(img: np.ndarray, k: np.ndarray) -> np.ndarray:
    """In-place scan like CInfraredCore::ReplaceBadPoint; img int16[H, W]."""
    bad = (k.reshape(H, W) & 0x8000) != 0
    if not bad.any():
        return img
    out = img.astype(np.int64)
    for y, x in zip(*np.nonzero(bad)):
        vals = []
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                yy, xx = y + dy, x + dx
                if 0 <= yy < H and 0 <= xx < W and not bad[yy, xx]:
                    vals.append(int(out[yy, xx]))
        if not vals:
            continue
        vals.sort(reverse=True)
        n = len(vals)
        if n % 2:
            out[y, x] = vals[n // 2]
        else:
            out[y, x] = int(trunc_div(np.int64(vals[n // 2 - 1]), np.int64(2))) + \
                int(trunc_div(np.int64(vals[n // 2]), np.int64(2)))
    return int16(out)


def rsn_filter(img: np.ndarray, table: np.ndarray, wx: int, wy: int):
    """FixedPoint_GrayFilter_16bit_RSN: range-weighted smoothing; returns (F int16, W uint16)."""
    hx, hy = wx // 2, wy // 2
    src = img.astype(np.int64)
    pad = np.pad(src, ((hy, hy), (hx, hx)), mode="edge")
    sw = np.zeros((H, W), np.int64)
    swv = np.zeros((H, W), np.int64)
    count = wx * wy
    for dy in range(wy):
        for dx in range(wx):
            v = pad[dy:dy + H, dx:dx + W]
            wgt = table[np.minimum(np.abs(src - v), 511)]
            sw += wgt
            swv += wgt * v
    f = np.where(sw >= 1, trunc_div(swv, np.where(sw >= 1, sw, 1)), src)
    wmean = (sw & 0xFFFFFFFF) // count
    return int16(f), (wmean & 0xFFFF).astype(np.uint16)


def remove_stripes(img: np.ndarray, table: np.ndarray, vertical: bool, win: int, thresh: int, clamp: int):
    if vertical:
        f, w = rsn_filter(img, table, win, 1)
    else:
        f, w = rsn_filter(img, table, 1, win)
    d = np.clip(img.astype(np.int64) - f.astype(np.int64), -clamp, clamp)
    use = w.astype(np.int64) >= thresh
    axis = 0 if vertical else 1
    s = np.where(use, d, 0).sum(axis=axis)
    n = use.sum(axis=axis)
    m = np.where(n >= 1, trunc_div(s, np.where(n >= 1, n, 1)), 0)
    m = np.clip(m, -clamp, clamp)
    corr = int16(m)  # applied as int16 subtraction
    if vertical:
        return int16(img.astype(np.int64) - corr[None, :].astype(np.int64))
    return int16(img.astype(np.int64) - corr[:, None].astype(np.int64))


def gauss3(img: np.ndarray) -> np.ndarray:
    pad = np.pad(img.astype(np.int64), 1, mode="edge")
    s = np.zeros((H, W), np.int64)
    for dy in range(3):
        for dx in range(3):
            s += GAUSS3[dy, dx] * pad[dy:dy + H, dx:dx + W]
    # (uint32)s >> 12, truncated to int16: equal to the arithmetic shift's low 16 bits
    return int16((s & 0xFFFFFFFF) >> 12)


def rotate_type2(img: np.ndarray) -> np.ndarray:
    return np.rot90(img, 1)


@dataclass
class Y16Model:
    package: Package
    table_v: np.ndarray = field(default_factory=lambda: range_weight_table(25))
    table_h: np.ndarray = field(default_factory=lambda: range_weight_table(25))
    b: np.ndarray = field(default_factory=lambda: np.zeros(N, np.int16))
    gear: int = 0

    def set_package(self, package: Package):
        """Switches the measuring range (guideCoreSetMeasureMode).

        The vendor core starts the next frame with K gear 0 again, as after
        guideCoreInit, and keeps B; measured against the vendor library by
        switching the range in the middle of a sequence.
        """
        self.package = package
        self.gear = 0

    def gear_for(self, fpa: int) -> int:
        f = self.package.focus
        n = len(f)
        if fpa < f[0]:
            return 0
        if fpa > f[n - 1]:
            return n
        for i in range(n - 1):
            if f[i] <= fpa <= f[i + 1]:
                return i + 1
        return 1

    def process(self, frame: bytes) -> np.ndarray:
        """Feeds one raw 25600-byte frame; returns Y16 int16[120, 90] (rows x cols)."""
        words = np.frombuffer(frame, "<i2")
        x = words[360:360 + N]
        k = self.package.k[self.gear]
        v = (x.view(np.uint16).astype(np.int64) - self.b.view(np.uint16).astype(np.int64)) * (k & 0x7FFF).astype(np.int64)
        img = int16(trunc_div(v, np.int64(8192))).reshape(H, W)
        img = replace_bad_points(img, k)
        img = remove_stripes(img, self.table_v, True, 9, 3200, 25)
        img = remove_stripes(img, self.table_h, False, 9, 3500, 20)
        img = gauss3(img)
        y16 = rotate_type2(img).copy()
        # after the image: shutter frame becomes the new B (guideCoreUpdateB is called
        # after ConvertXToImage), and the K gear follows the frame's FPA temperature
        if words[12] == 1:
            self.b = x.copy()
        self.gear = self.gear_for(int(words[11]))
        return y16
