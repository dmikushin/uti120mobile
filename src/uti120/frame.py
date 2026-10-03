"""Layout of one UTi120Mobile frame as delivered on the bulk endpoint.

A frame is 25600 bytes of little-endian 16-bit words:

    words [0, 120)                 header row (see the HDR_* indices below)
    words [120, 120 + 120*92)      sensor block, 92 rows x 120 columns, 14-bit
                                   rows 0..1 are reference rows (always 0x3FFF),
                                   rows 2..91 are the 90 active image rows
    words [11160, 12798)           zero padding
    bytes [25596, 25600)           CRC-32 (zlib polynomial) of bytes [0, 25596),
                                   stored little-endian

Header offsets are taken from FrameParamReader in the vendor Android app;
temperatures are stored in hundredths of a degree Celsius.
"""

import struct
import zlib
from dataclasses import dataclass

import numpy as np

FRAME_BYTES = 25600
WIDTH = 120
HEIGHT = 90
SENSOR_ROWS = 92
REFERENCE_ROWS = 2
MAGIC = 0xAA55

HDR_MAGIC = 0
HDR_FRAME_ID = 1
HDR_WIDTH = 2
HDR_HEIGHT = 3
HDR_SHUTTER_TEMP_INIT = 8
HDR_SHUTTER_TEMP = 9
HDR_TUBE_TEMP = 10
HDR_FPA_TEMP = 11
HDR_SHUTTER_CLOSED = 12
HDR_NUC_STATUS = 13


class FrameError(ValueError):
    pass


@dataclass
class Frame:
    words: np.ndarray  # uint16[12800], the whole frame

    @classmethod
    def parse(cls, data: bytes) -> "Frame":
        if len(data) != FRAME_BYTES:
            raise FrameError(f"frame has {len(data)} bytes, expected {FRAME_BYTES}")
        crc_stored = struct.unpack_from("<I", data, FRAME_BYTES - 4)[0]
        crc = zlib.crc32(data[:FRAME_BYTES - 4])
        if crc != crc_stored:
            raise FrameError(f"CRC mismatch: computed {crc:08x}, stored {crc_stored:08x}")
        words = np.frombuffer(data, dtype="<u2").copy()
        if words[HDR_MAGIC] != MAGIC:
            raise FrameError(f"bad magic {words[HDR_MAGIC]:04x}")
        if words[HDR_WIDTH] != WIDTH or words[HDR_HEIGHT] != SENSOR_ROWS:
            raise FrameError(f"unexpected geometry {words[HDR_WIDTH]}x{words[HDR_HEIGHT]}")
        return cls(words)

    @property
    def pixels(self) -> np.ndarray:
        """Active image, uint16[90, 120], raw 14-bit detector counts."""
        start = WIDTH * (1 + REFERENCE_ROWS)
        return self.words[start:start + WIDTH * HEIGHT].reshape(HEIGHT, WIDTH)

    @property
    def frame_id(self) -> int:
        return int(self.words[HDR_FRAME_ID])

    @property
    def shutter_closed(self) -> bool:
        return bool(self.words[HDR_SHUTTER_CLOSED])

    @property
    def nuc_status(self) -> int:
        return int(self.words[HDR_NUC_STATUS])

    def _temp(self, idx: int) -> float:
        return int(self.words[idx]) / 100.0

    @property
    def shutter_temp(self) -> float:
        return self._temp(HDR_SHUTTER_TEMP)

    @property
    def shutter_temp_init(self) -> float:
        return self._temp(HDR_SHUTTER_TEMP_INIT)

    @property
    def tube_temp(self) -> float:
        return self._temp(HDR_TUBE_TEMP)

    @property
    def fpa_temp(self) -> float:
        return self._temp(HDR_FPA_TEMP)

    def describe(self) -> str:
        return (f"id={self.frame_id} shutter={'closed' if self.shutter_closed else 'open'} "
                f"nuc={self.nuc_status} fpa={self.fpa_temp:.2f}C shutter={self.shutter_temp:.2f}C "
                f"tube={self.tube_temp:.2f}C")
