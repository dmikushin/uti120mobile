"""Turning raw detector counts into a picture.

The raw microbolometer image is dominated by per-pixel offsets (fixed pattern
noise) that are far larger than the scene contrast.  The shutter is a uniform
reference: the mean of a few closed-shutter frames is the per-pixel offset map,
and subtracting it from open-shutter frames leaves the scene signal.
"""

import warnings

import numpy as np

ADC_MAX = 0x3FFF


class Calibration:
    def __init__(self, dark: np.ndarray):
        self.dark = dark.astype(np.float32)
        self.bad = find_bad_pixels(self.dark)

    @classmethod
    def from_frames(cls, frames) -> "Calibration":
        return cls(np.mean([f.pixels for f in frames], axis=0))

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """Offset-corrected signal (float32, counts; warmer is larger)."""
        img = pixels.astype(np.float32) - self.dark
        bad = self.bad | (pixels == 0) | (pixels == ADC_MAX)
        if bad.any():
            img = replace_pixels(img, bad)
        return img


def find_bad_pixels(dark: np.ndarray, k: float = 8.0) -> np.ndarray:
    """Pixels whose closed-shutter level is saturated or far from their neighbours."""
    local = median3x3(dark)
    resid = dark - local
    mad = np.median(np.abs(resid - np.median(resid))) + 1e-6
    return (np.abs(resid) > k * 1.4826 * mad) | (dark <= 0) | (dark >= ADC_MAX)


def median3x3(img: np.ndarray) -> np.ndarray:
    p = np.pad(img, 1, mode="edge")
    stack = [p[dy:dy + img.shape[0], dx:dx + img.shape[1]] for dy in range(3) for dx in range(3)]
    return np.median(stack, axis=0)


def replace_pixels(img: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """Replace masked pixels by the median of their unmasked 3x3 neighbours."""
    p = np.pad(np.where(mask, np.nan, img), 1, mode="constant", constant_values=np.nan)
    stack = np.stack([p[dy:dy + img.shape[0], dx:dx + img.shape[1]]
                      for dy in range(3) for dx in range(3)])
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)  # all-NaN neighbourhoods
        fill = np.nanmedian(stack, axis=0)
    out = img.copy()
    out[mask] = np.nan_to_num(fill[mask], nan=float(np.nanmedian(img)))
    return out


class AutoGain:
    """Maps the signal to [0, 1] between low/high percentiles, smoothed over time."""

    def __init__(self, low: float = 1.0, high: float = 99.0, smoothing: float = 0.8):
        self.low, self.high, self.smoothing = low, high, smoothing
        self.range = None

    def __call__(self, img: np.ndarray) -> np.ndarray:
        lo, hi = np.percentile(img, [self.low, self.high])
        if self.range is None:
            self.range = np.array([lo, hi])
        else:
            self.range = self.smoothing * self.range + (1 - self.smoothing) * np.array([lo, hi])
        lo, hi = self.range
        return np.clip((img - lo) / max(hi - lo, 1.0), 0.0, 1.0)

