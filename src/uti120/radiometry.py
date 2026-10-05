"""Temperatures in degrees Celsius, computed the way the vendor app computes them.

The camera stores its calibration in flash: two packages (low and high
measuring range) holding per-pixel gains and counts-to-temperature curves, and
eight correction coefficients in system registers 49..56.  The vendor app reads
them once, then for every frame

  1. turns the raw frame into a Y16 image (vendor_y16: gain/offset correction
     against the last shutter frame, bad pixels, stripe removal, smoothing),
  2. maps each Y16 value to a temperature with the curves, corrected for the
     camera's own temperatures from the frame header (vendor_temp),
  3. applies a linear correction k*t + b chosen by temperature band
     (MainActivity.genCalibrationValue).

Steps 1 and 2 reproduce the vendor's native library bit for bit (checked
against it on recorded frames, see tests/test_radiometry.py); step 3 is its
Java code.
"""

from __future__ import annotations

import json
import logging
import os
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import vendor_temp, vendor_y16
from .device import CALIBRATION_HIGH, CALIBRATION_LOW, Camera, DeviceError

log = logging.getLogger(__name__)
PACKAGE_MAGIC = b"TI_CAL_METHOD"

LOW_RANGE_MAX = 150.0  # the vendor app switches to the high range above this
HIGH_RANGE_MIN = 120.0  # ... and back below this


@dataclass
class CameraCalibration:
    """Everything the camera stores for temperature measurement."""

    low: bytes                 # calibration package, low range (flash 0x132000)
    high: bytes                # calibration package, high range (flash 0x100000)
    coefficients: list         # system registers 49..56, k and b of four bands, x10000

    def save(self, directory: Path):
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "low.bin").write_bytes(self.low)
        (directory / "high.bin").write_bytes(self.high)
        (directory / "coefficients.json").write_text(json.dumps(self.coefficients))

    @classmethod
    def load(cls, directory: Path) -> CameraCalibration:
        return cls((directory / "low.bin").read_bytes(), (directory / "high.bin").read_bytes(),
                   json.loads((directory / "coefficients.json").read_text()))


def cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    return Path(base) / "uti120"


def calibrated_camera(cam: Camera, cache: Path | None = None):
    """Returns (CameraCalibration, Camera) for the connected camera.

    The calibration is read from the camera once and cached per sensor id.
    Reading it makes the firmware stop streaming until the camera reboots, so
    after a read the camera is rebooted and opened again; the returned Camera
    is the one to use.
    """
    sensor = cam.info()["sensor"]
    directory = (cache or cache_dir()) / sensor
    if (directory / "coefficients.json").exists():
        return CameraCalibration.load(directory), cam
    log.info("reading the calibration of camera %s (once; it is cached in %s)", sensor, directory)
    cal = CameraCalibration(cam.read_calibration_package(CALIBRATION_LOW),
                            cam.read_calibration_package(CALIBRATION_HIGH),
                            cam.read_calibration_coefficients())
    for name, package in (("low", cal.low), ("high", cal.high)):
        if PACKAGE_MAGIC not in package[:0x40]:
            raise DeviceError(f"{name} calibration package has an unknown format")
    cal.save(directory)
    cam.reboot()
    cam.close()
    return cal, _reopen()


def _reopen(timeout: float = 15.0) -> Camera:
    """Opens the camera after a reboot, once it has enumerated again."""
    deadline = time.monotonic() + timeout
    time.sleep(1.0)  # it disappears from the bus first
    while True:
        try:
            cam = Camera()
            if cam.info()["init_status"] == 1:
                return cam
            cam.close()
        except (DeviceError, OSError) as e:
            if time.monotonic() > deadline:
                raise DeviceError(f"camera did not come back after reboot: {e}") from None
        time.sleep(0.3)


@dataclass
class Settings:
    emissivity: float = 0.95   # vendor default (SRateBean)
    reflected: float = 23.0    # degrees C; the vendor core's default
    distance: float = 0.6      # metres; vendor default (ConfigBean.refDistant)
    high_range: bool = False   # measuring range: low (default) or high


def band_correction(t: np.ndarray, coefficients, high_range: bool) -> np.ndarray:
    """MainActivity.genCalibrationValue, per pixel, in float32 like the Java code."""
    c = np.asarray(coefficients, np.float32) / np.float32(10000.0)
    t = np.asarray(t, np.float32)
    k = np.ones_like(t)
    b = np.zeros_like(t)
    for (kk, bb), sel in (((c[0], c[1]), t <= 0),
                          ((c[2], c[3]), (t > 0) & (t <= 80)),
                          ((c[4], c[5]), (t > 80) & (t <= 150))):
        k = np.where(sel, kk, k)
        b = np.where(sel, bb, b)
    if high_range:
        hot = t > 100
        k = np.where(hot, c[6], k)
        b = np.where(hot, c[7], b)
    return np.where(k > 0, k * t + b, t).astype(np.float32)


@dataclass
class Radiometer:
    """Feed every frame in order (shutter frames included); open frames yield temperatures."""

    calibration: CameraCalibration
    settings: Settings = field(default_factory=Settings)

    def __post_init__(self):
        package = self.calibration.high if self.settings.high_range else self.calibration.low
        self.y16 = vendor_y16.Y16Model(vendor_y16.Package.parse(package))
        self.model = vendor_temp.TemperatureModel(
            vendor_temp.Package.parse(package),
            vendor_temp.Settings(self.settings.emissivity, self.settings.reflected, self.settings.distance),
            high=self.settings.high_range)
        self.state = vendor_temp.State()
        self.have_reference = False

    def feed(self, frame: bytes) -> bool:
        """Processes the next frame; True if temperatures() can be asked for it.

        False for shutter frames and until the first shutter frame has been seen
        (the vendor app does not measure before its first shutter/NUC either).
        Every frame must be fed, in order: the shutter frames are the reference
        and the frame headers carry the camera temperatures the curves depend on.
        """
        words = np.frombuffer(frame, "<i2", 16)
        self._y16 = np.rot90(self.y16.process(frame), -1)   # back to the sensor's 120 x 90
        self.state.update(words)
        if words[12] == 1:
            self.have_reference = True
            return False
        return self.have_reference

    def temperatures(self) -> np.ndarray:
        """float32[90, 120] degrees C of the last frame fed, in sensor orientation."""
        # Within a frame the temperature depends on the Y16 value only.
        values, index = np.unique(self._y16, return_inverse=True)
        t = np.array([self.model.temperature(int(v), self.state) for v in values], np.float32)
        t = band_correction(t, self.calibration.coefficients, self.settings.high_range)
        return t[index].reshape(self._y16.shape)
