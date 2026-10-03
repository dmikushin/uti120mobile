"""USB protocol of the UNI-T UTi120Mobile.

Recovered from the vendor Android app (package com.unit.usblib.armlib,
class UnitArmInterface_ByGuide):

* interface 0 carries frames on bulk IN 0x82;
* interface 1 carries commands: a request is written to interrupt OUT 0x01
  and the reply is read from interrupt IN 0x81.

Command layout: [function, register offset, register count, value bytes...],
values are 32-bit big-endian.  A read reply is [function, byte count, values...];
a write reply echoes [function, offset, count].

Streaming: set the run-status register to 2, then for every frame write the
single byte 0x81 to the command endpoint and read 25600 bytes from bulk IN.
"""

import logging

import usb.core
import usb.util

from .frame import FRAME_BYTES, Frame, FrameError

log = logging.getLogger(__name__)

VID = 0x5656
PID = 0x1201

EP_BULK_IN = 0x82
EP_CMD_OUT = 0x01
EP_CMD_IN = 0x81

# Function codes.
SYS_WRITE = 0x04
SYS_READ = 0x05
SENSOR_WRITE = 0x0A
SENSOR_READ = 0x0B
REQUEST_FRAME = 0x81

# System registers (SYS_READ / SYS_WRITE).
REG_FACTORY_ID = 0x00
REG_PRODUCT_ID = 0x01
REG_HW_VERSION = 0x02
REG_SW_VERSION = 0x03
REG_SENSOR_ID = 0x07  # 5 registers, ASCII
REG_REBOOT = 0xE0
REG_RUN_STATUS = 0xF0
REG_INIT_STATUS = 0xF2

# Sensor registers (SENSOR_READ / SENSOR_WRITE).
SREG_SHUTTER = 0x03  # 1 = closed, 0 = open
SREG_NUC = 0x04      # write 1 to trigger the on-camera offset calibration

RUN_IDLE = 0
RUN_STREAM = 2

CMD_TIMEOUT_MS = 300
CHUNK = 4096
FIRST_CHUNK_TIMEOUT_MS = 50
CHUNK_TIMEOUT_MS = 200
DRAIN_TIMEOUT_MS = 10


class DeviceError(RuntimeError):
    pass


class Camera:
    def __init__(self):
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is None:
            raise DeviceError(f"no USB device {VID:04x}:{PID:04x} found")
        # Deliberately no set_configuration(): the device enumerates already
        # configured, and re-selecting the configuration resets the host-side
        # data toggles only, after which the command endpoint stops answering.
        for intf in (0, 1):
            if dev.is_kernel_driver_active(intf):
                dev.detach_kernel_driver(intf)
            usb.util.claim_interface(dev, intf)
        self.dev = dev
        self.last_frame_id = None

    def close(self):
        usb.util.dispose_resources(self.dev)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- command channel -------------------------------------------------

    def _drain_cmd(self):
        while True:
            try:
                stale = bytes(self.dev.read(EP_CMD_IN, 64, 1))
                log.debug("discarded stale reply %s", stale.hex())
            except usb.core.USBTimeoutError:
                return

    def _transact(self, request: bytes) -> bytes:
        # A reply that arrived after an earlier timeout must not be taken for
        # this request's reply (two shutter writes have identical replies).
        self._drain_cmd()
        self.dev.write(EP_CMD_OUT, request, CMD_TIMEOUT_MS)
        try:
            reply = bytes(self.dev.read(EP_CMD_IN, 64, CMD_TIMEOUT_MS))
        except usb.core.USBTimeoutError:
            raise DeviceError(f"no reply to command {request.hex()}") from None
        log.debug("cmd %s -> %s", request.hex(), reply.hex())
        return reply

    def read_regs(self, func: int, offset: int, count: int = 1) -> list[int]:
        reply = self._transact(bytes([func, offset, count]))
        if len(reply) < 2 + 4 * count or reply[0] != func or reply[1] != 4 * count:
            raise DeviceError(f"bad reply to read {func:02x}/{offset:02x}: {reply.hex()}")
        return [int.from_bytes(reply[2 + 4 * i:6 + 4 * i], "big") for i in range(count)]

    def write_reg(self, func: int, offset: int, value: int):
        reply = self._transact(bytes([func, offset, 1]) + value.to_bytes(4, "big"))
        if reply[:3] != bytes([func, offset, 1]):
            raise DeviceError(f"bad reply to write {func:02x}/{offset:02x}: {reply.hex()}")

    def info(self) -> dict:
        sw = self.read_regs(SYS_READ, REG_SW_VERSION)[0]
        hw = self.read_regs(SYS_READ, REG_HW_VERSION)[0]
        sensor = b"".join(v.to_bytes(4, "big") for v in self.read_regs(SYS_READ, REG_SENSOR_ID, 5))
        return {
            "firmware": f"{(sw >> 16) & 0xFF}.{(sw >> 8) & 0xFF}.{sw & 0xFF} build {sw >> 24}",
            "hardware": f"{(hw >> 16) & 0xFF}.{(hw >> 8) & 0xFF}.{hw & 0xFF}",
            "sensor": sensor.rstrip(b"\0").decode("ascii", "replace"),
            "run_status": self.read_regs(SYS_READ, REG_RUN_STATUS)[0],
            "init_status": self.read_regs(SYS_READ, REG_INIT_STATUS)[0],
        }

    def set_run_status(self, status: int):
        self.write_reg(SYS_WRITE, REG_RUN_STATUS, status)

    def set_shutter(self, closed: bool):
        self.write_reg(SENSOR_WRITE, SREG_SHUTTER, int(closed))

    def nuc(self):
        self.write_reg(SENSOR_WRITE, SREG_NUC, 1)

    # -- frames -----------------------------------------------------------

    def _drain_bulk(self):
        # The timeout doubles as the pause the camera needs between frame
        # requests: a request sent sooner after the previous frame is often
        # ignored (measured: 0 ms gap -> 13 fps, 10 ms -> 23 fps, 20 ms -> 20 fps).
        while True:
            try:
                n = len(self.dev.read(EP_BULK_IN, CHUNK, DRAIN_TIMEOUT_MS))
                log.debug("discarded %d stale bulk bytes", n)
            except usb.core.USBTimeoutError:
                return

    def _read_frame_once(self) -> bytes:
        self._drain_bulk()
        self.dev.write(EP_CMD_OUT, bytes([REQUEST_FRAME]), CMD_TIMEOUT_MS)
        # The first chunk normally arrives ~8 ms after the request; an ignored
        # request is detected by a short timeout and simply repeated.
        buf = bytearray(self.dev.read(EP_BULK_IN, CHUNK, FIRST_CHUNK_TIMEOUT_MS))
        while len(buf) < FRAME_BYTES:
            buf += bytes(self.dev.read(EP_BULK_IN, CHUNK, CHUNK_TIMEOUT_MS))
        return bytes(buf)

    def grab(self, retries: int = 50) -> Frame:
        """Request and return one frame.

        Right after streaming is enabled, and right after shutter or NUC
        commands, the camera ignores frame requests for a short while; those
        requests time out and are repeated.
        """
        for attempt in range(retries):
            try:
                f = Frame.parse(self._read_frame_once())
                # A frame answering an earlier, timed-out request may still
                # arrive; only frames newer than the last one are accepted
                # (the 16-bit counter wraps).
                if (self.last_frame_id is not None
                        and not 0 < (f.frame_id - self.last_frame_id) % 0x10000 < 0x8000):
                    log.warning("dropping stale frame %d (last %d)", f.frame_id, self.last_frame_id)
                    continue
                self.last_frame_id = f.frame_id
                return f
            except usb.core.USBTimeoutError:
                log.debug("frame request %d timed out", attempt)
            except FrameError as e:
                log.warning("dropping frame: %s", e)
        raise DeviceError(f"no valid frame after {retries} requests")

    def start(self):
        self.set_run_status(RUN_STREAM)
        # The first frame after (re)starting is the one the camera prepared
        # when streaming last stopped, possibly long ago (measured: always the
        # previous session's last frame id + 1, while the 16-bit counter keeps
        # running at 25 Hz and wraps every ~44 min).  Discard it and start the
        # frame-order check afresh.
        self.last_frame_id = None
        self.grab()
        self.last_frame_id = None

    def stop(self):
        self.set_run_status(RUN_IDLE)
