"""Offline tests on frames recorded from a real UTi120Mobile (tests/data)."""

import gzip
import struct
import zlib
from pathlib import Path

import numpy as np
import pytest
import usb.core

from uti120 import palette
from uti120.device import Camera, DeviceError
from uti120.frame import FRAME_BYTES, Frame, FrameError
from uti120.process import Calibration

DATA = Path(__file__).parent / "data"


def load(name):
    raw = gzip.open(DATA / name).read()
    return [raw[i:i + FRAME_BYTES] for i in range(0, len(raw), FRAME_BYTES)]


@pytest.fixture(scope="module")
def closed():
    return [Frame.parse(b) for b in load("closed16.bin.gz")]


@pytest.fixture(scope="module")
def opened():
    return [Frame.parse(b) for b in load("open4.bin.gz")]


def test_header(closed, opened):
    assert all(f.shutter_closed for f in closed)
    assert not any(f.shutter_closed for f in opened)
    ids = [f.frame_id for f in closed]
    assert all(b > a for a, b in zip(ids, ids[1:]))
    for f in closed + opened:
        assert f.pixels.shape == (90, 120)
        assert 0 < f.fpa_temp < 60


def test_crc_and_length_are_checked():
    raw = bytearray(load("open4.bin.gz")[0])
    raw[1000] ^= 1
    with pytest.raises(FrameError, match="CRC"):
        Frame.parse(bytes(raw))
    with pytest.raises(FrameError, match="bytes"):
        Frame.parse(bytes(raw[:-1]))
    # A frame with a valid CRC but wrong magic is still rejected.
    raw = bytearray(load("open4.bin.gz")[0])
    raw[0] = 0
    struct.pack_into("<I", raw, FRAME_BYTES - 4, zlib.crc32(raw[:FRAME_BYTES - 4]))
    with pytest.raises(FrameError, match="magic"):
        Frame.parse(bytes(raw))


def test_shutter_calibration_removes_fixed_pattern(closed, opened):
    cal = Calibration.from_frames(closed[:8])
    # Raw detector output is dominated by per-pixel offsets...
    raw_spread = closed[8].pixels.astype(float).std()
    # ...which the closed-shutter reference removes, leaving temporal noise only.
    corrected_dark = cal.apply(closed[8].pixels)
    assert raw_spread > 5 * corrected_dark.std()
    # The scene (a running motherboard, warmer than the shutter) is well above that noise.
    scene = np.mean([cal.apply(f.pixels) for f in opened], axis=0)
    assert scene.std() > 5 * corrected_dark.std()
    assert np.median(scene) > 0


def test_bad_pixels_are_replaced(closed):
    cal = Calibration.from_frames(closed)
    px = closed[0].pixels.copy()
    px[40, 60] = 0x3FFF
    out = cal.apply(px)
    assert abs(out[40, 60] - np.median(out[39:42, 59:62])) < 100


def test_palette():
    for name in palette.NAMES:
        lut = palette.lut(name)
        assert lut.shape == (256, 3) and lut.dtype == np.uint8
    rgb = palette.colorize(np.array([[0.0, 1.0]]), "grey")
    assert rgb.tolist() == [[[0, 0, 0], [255, 255, 255]]]


class FakeUsb:
    """Replays scripted replies for the command and bulk endpoints."""

    def __init__(self, cmd_replies=(), bulk=()):
        self.cmd_replies = list(cmd_replies)
        self.bulk = list(bulk)
        self.written = []

    def write(self, ep, data, timeout):
        self.written.append(bytes(data))
        return len(data)

    def read(self, ep, size, timeout):
        queue = self.cmd_replies if ep == 0x81 else self.bulk
        item = queue.pop(0) if queue else None
        if item is None:
            raise usb.core.USBTimeoutError("timeout", -7, 110)
        return item[:size]


def fake_camera(**kw):
    cam = Camera.__new__(Camera)
    cam.dev = FakeUsb(**kw)
    return cam


def test_register_protocol():
    cam = fake_camera(cmd_replies=[bytes.fromhex("050407010103"), bytes.fromhex("0a0301")])
    assert cam.read_regs(0x05, 0x03) == [0x07010103]
    cam.set_shutter(True)
    assert cam.dev.written == [bytes.fromhex("050301"), bytes.fromhex("0a03010000000" + "1")]


def test_register_errors():
    with pytest.raises(DeviceError, match="no reply"):
        fake_camera().read_regs(0x05, 0x07)
    with pytest.raises(DeviceError, match="bad reply"):
        fake_camera(cmd_replies=[bytes.fromhex("0a0401")]).set_shutter(False)


def test_grab_repeats_ignored_requests():
    frame = load("open4.bin.gz")[0]
    chunks = [frame[i:i + 4096] for i in range(0, FRAME_BYTES, 4096)]
    # bulk script: drain sees nothing, first request ignored (timeout), drain, second answered
    cam = fake_camera(bulk=[None, None, None] + chunks)
    f = cam.grab()
    assert f.words.tobytes() == frame
    assert cam.dev.written == [b"\x81", b"\x81"]
