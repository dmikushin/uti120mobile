// USB protocol of the UNI-T UTi120Mobile.
//
// Recovered from the vendor Android app (package com.unit.usblib.armlib,
// class UnitArmInterface_ByGuide):
//
// * interface 0 carries frames on bulk IN 0x82;
// * interface 1 carries commands: a request is written to interrupt OUT 0x01
//   and the reply is read from interrupt IN 0x81.
//
// Command layout: [function, register offset, register count, value bytes...],
// values are 32-bit big-endian.  A read reply is [function, byte count, values...];
// a write reply echoes [function, offset, count].
//
// Streaming: set the run-status register to 2, then for every frame write the
// single byte 0x81 to the command endpoint and read 25600 bytes from bulk IN.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "uti120/frame.hpp"

namespace uti120 {

constexpr uint16_t VID = 0x5656;
constexpr uint16_t PID = 0x1201;

constexpr uint8_t EP_BULK_IN = 0x82;
constexpr uint8_t EP_CMD_OUT = 0x01;
constexpr uint8_t EP_CMD_IN = 0x81;

// Function codes.
constexpr uint8_t SYS_WRITE = 0x04;
constexpr uint8_t SYS_READ = 0x05;
constexpr uint8_t SENSOR_WRITE = 0x0A;
constexpr uint8_t SENSOR_READ = 0x0B;
constexpr uint8_t REQUEST_FRAME = 0x81;
constexpr uint8_t TRANSFER = 0x09;  // flash upload: begin / block ack / end

// System registers (SYS_READ / SYS_WRITE).
constexpr uint8_t REG_FACTORY_ID = 0x00;
constexpr uint8_t REG_PRODUCT_ID = 0x01;
constexpr uint8_t REG_HW_VERSION = 0x02;
constexpr uint8_t REG_SW_VERSION = 0x03;
constexpr uint8_t REG_SENSOR_ID = 0x07;  // 5 registers, ASCII
constexpr uint8_t REG_REBOOT = 0xE0;
constexpr uint8_t REG_RUN_STATUS = 0xF0;
constexpr uint8_t REG_INIT_STATUS = 0xF2;

// Sensor registers (SENSOR_READ / SENSOR_WRITE).
constexpr uint8_t SREG_SHUTTER = 0x03;  // 1 = closed, 0 = open
constexpr uint8_t SREG_NUC = 0x04;      // write 1 to trigger the on-camera offset calibration

constexpr uint32_t RUN_IDLE = 0;
constexpr uint32_t RUN_STREAM = 2;
constexpr uint32_t RUN_UPLOAD = 3;

// Calibration packages stored in the camera's flash, read by the vendor app at
// start-up and handed to its temperature code (UnitArmInterface_ByGuide.getGuiderPackage).
enum class CalibrationPackage {
  Low = 0,   // address 0x132000, length in system register 13
  High = 1,  // address 0x100000, length in system register 12
};

constexpr unsigned CMD_TIMEOUT_MS = 300;
constexpr int CHUNK = 4096;
constexpr unsigned FIRST_CHUNK_TIMEOUT_MS = 50;
constexpr unsigned CHUNK_TIMEOUT_MS = 200;
constexpr unsigned DRAIN_TIMEOUT_MS = 10;

struct DeviceError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Endpoint I/O.  Implemented over libusb for the real camera and by a script
// in the tests.
class Transport {
 public:
  virtual ~Transport() = default;
  virtual void write(uint8_t ep, const std::vector<uint8_t>& data, unsigned timeout_ms) = 0;
  // Returns the bytes received (at most max_len), or nullopt on timeout.
  virtual std::optional<std::vector<uint8_t>> read(uint8_t ep, int max_len,
                                                   unsigned timeout_ms) = 0;
};

// Opens the first 5656:1201 device and claims both interfaces.
std::unique_ptr<Transport> open_usb();
// Same for a device already opened by the platform (Android's
// UsbDeviceConnection.getFileDescriptor()); the descriptor stays owned by the caller.
std::unique_ptr<Transport> open_usb_fd(int fd);

// Where the time of delivered frames goes; cumulative since the camera was
// opened.  Times are sums in milliseconds; divide by `frames`.
struct GrabStats {
  long frames = 0;    // valid frames returned
  long ignored = 0;   // frame requests the camera did not answer in time
  long dropped = 0;   // frames rejected (stale, CRC, geometry)
  long reads = 0;     // bulk reads of delivered frames
  double drain_ms = 0;     // discarding stale data, includes the pause between requests
  double first_ms = 0;     // request until the first bulk read returned
  double transfer_ms = 0;  // first read until the frame was complete
  double total_ms = 0;     // whole grab(), including ignored requests and drops
};

struct DeviceInfo {
  std::string firmware, hardware, sensor;
  uint32_t run_status, init_status;
};

class Camera {
 public:
  explicit Camera(std::unique_ptr<Transport> transport);

  std::vector<uint32_t> read_regs(uint8_t func, uint8_t offset, int count = 1);
  void write_reg(uint8_t func, uint8_t offset, uint32_t value);

  DeviceInfo info();
  void set_run_status(uint32_t status) { write_reg(SYS_WRITE, REG_RUN_STATUS, status); }
  void set_shutter(bool closed) { write_reg(SENSOR_WRITE, SREG_SHUTTER, closed ? 1 : 0); }
  void nuc() { write_reg(SENSOR_WRITE, SREG_NUC, 1); }

  // Request and return one frame.  Right after streaming is enabled, and right
  // after shutter or NUC commands, the camera ignores frame requests for a
  // short while; those requests time out and are repeated.
  Frame grab(int retries = 50);

  void start();
  void stop() { set_run_status(RUN_IDLE); }

  // Reads a calibration package from the camera's flash.  The camera must not
  // be streaming; it is left idle.  Every 4096-byte block is acknowledged with
  // its CRC-32 and the whole package is checked against its CRC-32.
  std::vector<uint8_t> read_calibration(CalibrationPackage which);

  Transport& transport() { return *transport_; }
  std::optional<int> last_frame_id;

  // Thread-safe copy; grab() runs on the capture thread.
  GrabStats stats() const;

 private:
  void drain_cmd();
  std::vector<uint8_t> transact(const std::vector<uint8_t>& request);
  void drain_bulk();
  std::optional<std::vector<uint8_t>> read_frame_once();

  std::unique_ptr<Transport> transport_;
  mutable std::mutex stats_mutex_;
  GrabStats stats_;
};

std::string hex(const std::vector<uint8_t>& bytes);

}  // namespace uti120
