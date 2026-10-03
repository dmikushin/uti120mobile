"""Calibrated frame stream: start the camera, calibrate on the shutter, yield images."""

import logging
import time
from dataclasses import dataclass

import numpy as np

from .device import Camera
from .frame import Frame
from .process import Calibration

log = logging.getLogger(__name__)

# The vendor app ignores frames for 3 s after a NUC (checkNuc in DataInterface_ToGuide_Imp).
NUC_SETTLE_S = 3.0
# Frames still in flight with the previous shutter state after it reports the new one.
SHUTTER_SETTLE_FRAMES = 2


@dataclass
class Image:
    frame: Frame
    signal: np.ndarray  # float32[90, 120], offset-corrected counts
    timestamp: float


class Stream:
    def __init__(self, cam: Camera, nuc: bool = True, dark_frames: int = 16,
                 recalibrate_s: float = 0.0, raw_sink=None):
        self.cam = cam
        self.nuc = nuc
        self.dark_frames = dark_frames
        self.recalibrate_s = recalibrate_s
        self.raw_sink = raw_sink
        self.calibration = None
        self.calibrated_at = 0.0

    def _grab(self) -> Frame:
        f = self.cam.grab()
        if self.raw_sink is not None:
            self.raw_sink.write(f.words.tobytes())
        return f

    def start(self):
        self.cam.start()
        f = self._grab()
        log.info("streaming: %s", f.describe())
        if self.nuc:
            log.info("on-camera NUC")
            self.cam.nuc()
            end = time.monotonic() + NUC_SETTLE_S
            while time.monotonic() < end:
                self._grab()
        self.calibrate()

    def _wait_shutter(self, closed: bool):
        for _ in range(30):
            if self._grab().shutter_closed == closed:
                break
        else:
            raise RuntimeError(f"shutter never reported {'closed' if closed else 'open'}")
        for _ in range(SHUTTER_SETTLE_FRAMES):
            self._grab()

    def calibrate(self):
        self.cam.set_shutter(True)
        self._wait_shutter(True)
        frames = [self._grab() for _ in range(self.dark_frames)]
        self.cam.set_shutter(False)
        self._wait_shutter(False)
        self.calibration = Calibration.from_frames(frames)
        self.calibrated_at = time.monotonic()
        log.info("calibrated on %d shutter frames, %d bad pixels, fpa %.2fC",
                 len(frames), int(self.calibration.bad.sum()), frames[-1].fpa_temp)

    def read(self) -> Image:
        if self.recalibrate_s and time.monotonic() - self.calibrated_at > self.recalibrate_s:
            self.calibrate()
        f = self._grab()
        return Image(f, self.calibration.apply(f.pixels), time.time())

    def stop(self):
        try:
            self.cam.stop()
        finally:
            self.cam.close()
