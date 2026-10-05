#include "uti120/device.hpp"

#include <libusb.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "uti120/log.hpp"

namespace uti120 {

static constexpr const char* LOG = "uti120.device";

std::string hex(const std::vector<uint8_t>& bytes) {
  std::string s;
  char b[3];
  for (uint8_t v : bytes) {
    std::snprintf(b, sizeof b, "%02x", v);
    s += b;
  }
  return s;
}

namespace {

class LibusbTransport final : public Transport {
 public:
  // Finds the camera by VID:PID.
  LibusbTransport() {
    if (int rc = libusb_init_context(&ctx_, nullptr, 0); rc != 0)
      throw DeviceError(std::string("libusb_init: ") + libusb_error_name(rc));
    handle_ = libusb_open_device_with_vid_pid(ctx_, VID, PID);
    if (!handle_) {
      libusb_exit(ctx_);
      char msg[64];
      std::snprintf(msg, sizeof msg, "no USB device %04x:%04x found (or no permission)", VID, PID);
      throw DeviceError(msg);
    }
    claim();
  }

  // Uses an already opened usbfs file descriptor (Android: UsbDeviceConnection).
  // Device discovery is disabled: an app may not scan /dev/bus/usb.
  explicit LibusbTransport(int fd) {
    libusb_init_option opt{LIBUSB_OPTION_NO_DEVICE_DISCOVERY, {0}};
    if (int rc = libusb_init_context(&ctx_, &opt, 1); rc != 0)
      throw DeviceError(std::string("libusb_init: ") + libusb_error_name(rc));
    if (int rc = libusb_wrap_sys_device(ctx_, intptr_t(fd), &handle_); rc != 0) {
      libusb_exit(ctx_);
      throw DeviceError(std::string("libusb_wrap_sys_device: ") + libusb_error_name(rc));
    }
    claim();
  }

  void claim() {
    // Deliberately no libusb_set_configuration(): the device enumerates already
    // configured, and re-selecting the configuration resets the host-side data
    // toggles only, after which the command endpoint stops answering.
    libusb_set_auto_detach_kernel_driver(handle_, 1);
    for (int intf : {0, 1}) {
      if (int rc = libusb_claim_interface(handle_, intf); rc != 0) {
        for (int i = 0; i < intf; ++i) libusb_release_interface(handle_, i);
        libusb_close(handle_);
        libusb_exit(ctx_);
        throw DeviceError(std::string("claim interface: ") + libusb_error_name(rc));
      }
    }
  }

  ~LibusbTransport() override {
    libusb_release_interface(handle_, 0);
    libusb_release_interface(handle_, 1);
    libusb_close(handle_);
    libusb_exit(ctx_);
  }

  void write(uint8_t ep, const std::vector<uint8_t>& data, unsigned timeout_ms) override {
    int done = 0;
    int rc = transfer(ep, const_cast<uint8_t*>(data.data()), int(data.size()), &done, timeout_ms);
    if (rc != 0 || done != int(data.size()))
      throw DeviceError(std::string("USB write to ep ") + std::to_string(ep) + ": " +
                        libusb_error_name(rc));
  }

  std::optional<std::vector<uint8_t>> read(uint8_t ep, int max_len, unsigned timeout_ms) override {
    std::vector<uint8_t> buf(max_len);
    int done = 0;
    int rc = transfer(ep, buf.data(), max_len, &done, timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT && done == 0) return std::nullopt;
    if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT)
      throw DeviceError(std::string("USB read from ep ") + std::to_string(ep) + ": " +
                        libusb_error_name(rc));
    buf.resize(done);
    return buf;
  }

 private:
  int transfer(uint8_t ep, uint8_t* data, int len, int* done, unsigned timeout_ms) {
    // Interface 1 endpoints are interrupt endpoints, interface 0 is bulk.
    if (ep == EP_CMD_OUT || ep == EP_CMD_IN)
      return libusb_interrupt_transfer(handle_, ep, data, len, done, timeout_ms);
    return libusb_bulk_transfer(handle_, ep, data, len, done, timeout_ms);
  }

  libusb_context* ctx_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
};

uint32_t load_be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

}  // namespace

std::unique_ptr<Transport> open_usb() { return std::make_unique<LibusbTransport>(); }

std::unique_ptr<Transport> open_usb_fd(int fd) { return std::make_unique<LibusbTransport>(fd); }

Camera::Camera(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {}

void Camera::drain_cmd() {
  while (auto stale = transport_->read(EP_CMD_IN, 64, 1))
    LOG_DEBUG(LOG, "discarded stale reply %s", hex(*stale).c_str());
}

std::vector<uint8_t> Camera::transact(const std::vector<uint8_t>& request) {
  // A reply that arrived after an earlier timeout must not be taken for this
  // request's reply (two shutter writes have identical replies).
  drain_cmd();
  transport_->write(EP_CMD_OUT, request, CMD_TIMEOUT_MS);
  auto reply = transport_->read(EP_CMD_IN, 64, CMD_TIMEOUT_MS);
  if (!reply) throw DeviceError("no reply to command " + hex(request));
  LOG_DEBUG(LOG, "cmd %s -> %s", hex(request).c_str(), hex(*reply).c_str());
  return *reply;
}

std::vector<uint32_t> Camera::read_regs(uint8_t func, uint8_t offset, int count) {
  auto reply = transact({func, offset, uint8_t(count)});
  if (reply.size() < size_t(2 + 4 * count) || reply[0] != func || reply[1] != 4 * count) {
    char msg[64];
    std::snprintf(msg, sizeof msg, "bad reply to read %02x/%02x: ", func, offset);
    throw DeviceError(msg + hex(reply));
  }
  std::vector<uint32_t> values(count);
  for (int i = 0; i < count; ++i) values[i] = load_be32(&reply[2 + 4 * i]);
  return values;
}

void Camera::write_reg(uint8_t func, uint8_t offset, uint32_t value) {
  auto reply = transact({func, offset, 1, uint8_t(value >> 24), uint8_t(value >> 16),
                         uint8_t(value >> 8), uint8_t(value)});
  if (reply.size() < 3 || reply[0] != func || reply[1] != offset || reply[2] != 1) {
    char msg[64];
    std::snprintf(msg, sizeof msg, "bad reply to write %02x/%02x: ", func, offset);
    throw DeviceError(msg + hex(reply));
  }
}

DeviceInfo Camera::info() {
  uint32_t sw = read_regs(SYS_READ, REG_SW_VERSION)[0];
  uint32_t hw = read_regs(SYS_READ, REG_HW_VERSION)[0];
  std::string sensor;
  for (uint32_t v : read_regs(SYS_READ, REG_SENSOR_ID, 5))
    for (int shift = 24; shift >= 0; shift -= 8) sensor += char((v >> shift) & 0xFF);
  while (!sensor.empty() && sensor.back() == '\0') sensor.pop_back();
  char fw[48], hwv[32];
  std::snprintf(fw, sizeof fw, "%u.%u.%u build %u", (sw >> 16) & 0xFF, (sw >> 8) & 0xFF,
                sw & 0xFF, sw >> 24);
  std::snprintf(hwv, sizeof hwv, "%u.%u.%u", (hw >> 16) & 0xFF, (hw >> 8) & 0xFF, hw & 0xFF);
  return {fw, hwv, sensor, read_regs(SYS_READ, REG_RUN_STATUS)[0],
          read_regs(SYS_READ, REG_INIT_STATUS)[0]};
}

void Camera::drain_bulk() {
  // The timeout doubles as the pause the camera needs between frame requests:
  // a request sent sooner after the previous frame is often ignored
  // (measured: 0 ms gap -> 13 fps, 10 ms -> 23 fps, 20 ms -> 20 fps).
  while (auto stale = transport_->read(EP_BULK_IN, CHUNK, DRAIN_TIMEOUT_MS))
    LOG_DEBUG(LOG, "discarded %zu stale bulk bytes", stale->size());
}

namespace {
using Clock = std::chrono::steady_clock;
double ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}
}  // namespace

GrabStats Camera::stats() const {
  std::lock_guard lock(stats_mutex_);
  return stats_;
}

std::optional<std::vector<uint8_t>> Camera::read_frame_once() {
  auto t0 = Clock::now();
  drain_bulk();
  auto t1 = Clock::now();
  transport_->write(EP_CMD_OUT, {REQUEST_FRAME}, CMD_TIMEOUT_MS);
  // The first chunk normally arrives ~8 ms after the request; an ignored
  // request is detected by a short timeout and simply repeated.
  auto first = transport_->read(EP_BULK_IN, CHUNK, FIRST_CHUNK_TIMEOUT_MS);
  auto t2 = Clock::now();
  if (!first) {
    std::lock_guard lock(stats_mutex_);
    stats_.ignored++;
    return std::nullopt;
  }
  std::vector<uint8_t> buf = std::move(*first);
  long reads = 1;
  while (buf.size() < FRAME_BYTES) {
    auto chunk = transport_->read(EP_BULK_IN, CHUNK, CHUNK_TIMEOUT_MS);
    if (!chunk) {
      std::lock_guard lock(stats_mutex_);
      stats_.ignored++;
      return std::nullopt;
    }
    ++reads;
    buf.insert(buf.end(), chunk->begin(), chunk->end());
  }
  auto t3 = Clock::now();
  std::lock_guard lock(stats_mutex_);
  stats_.reads += reads;
  stats_.drain_ms += ms_between(t0, t1);
  stats_.first_ms += ms_between(t1, t2);
  stats_.transfer_ms += ms_between(t2, t3);
  return buf;
}

Frame Camera::grab(int retries) {
  auto start = Clock::now();
  auto account = [&](bool delivered) {
    std::lock_guard lock(stats_mutex_);
    stats_.total_ms += ms_between(start, Clock::now());
    if (delivered) stats_.frames++;
  };
  for (int attempt = 0; attempt < retries; ++attempt) {
    auto data = read_frame_once();
    if (!data) {
      LOG_DEBUG(LOG, "frame request %d timed out", attempt);
      continue;
    }
    try {
      Frame f = Frame::parse(*data);
      // A frame answering an earlier, timed-out request may still arrive;
      // only frames newer than the last one are accepted (the 16-bit counter
      // wraps).
      if (last_frame_id) {
        int d = (f.frame_id() - *last_frame_id) & 0xFFFF;
        if (!(0 < d && d < 0x8000)) {
          LOG_WARNING(LOG, "dropping stale frame %d (last %d)", f.frame_id(), *last_frame_id);
          std::lock_guard lock(stats_mutex_);
          stats_.dropped++;
          continue;
        }
      }
      last_frame_id = f.frame_id();
      account(true);
      return f;
    } catch (const FrameError& e) {
      LOG_WARNING(LOG, "dropping frame: %s", e.what());
      std::lock_guard lock(stats_mutex_);
      stats_.dropped++;
    }
  }
  account(false);
  throw DeviceError("no valid frame after " + std::to_string(retries) + " requests");
}

std::vector<uint8_t> Camera::read_calibration(CalibrationPackage which) {
  // Protocol of the vendor's UploadThread_ParamPkg.
  const bool high = which == CalibrationPackage::High;
  const uint32_t address = high ? 0x100000 : 0x132000;
  const uint32_t length = read_regs(SYS_READ, high ? 12 : 13)[0];
  if (length == 0 || length > 16 * 1024 * 1024)
    throw DeviceError("implausible calibration package length " + std::to_string(length));
  auto be32 = [](uint32_t v) {
    return std::vector<uint8_t>{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  };
  auto transfer = [&](uint8_t offset, std::vector<uint8_t> values) {
    std::vector<uint8_t> req{TRANSFER, offset, uint8_t(values.size() / 4)};
    req.insert(req.end(), values.begin(), values.end());
    auto reply = transact(req);
    if (reply.size() < 3 || reply[0] != req[0] || reply[1] != req[1] || reply[2] != req[2])
      throw DeviceError("bad reply to transfer command " + hex(req) + ": " + hex(reply));
  };

  // Stop the camera first, then discard whatever is still in flight (the rest
  // of a frame of an interrupted stream), so nothing stale precedes the package.
  set_run_status(RUN_IDLE);
  while (auto stale = transport_->read(EP_BULK_IN, CHUNK, 100))
    LOG_DEBUG(LOG, "discarded %zu stale bulk bytes before the upload", stale->size());
  set_run_status(RUN_UPLOAD);
  {
    auto v = be32(address);
    auto n = be32(length);
    v.insert(v.end(), n.begin(), n.end());
    transfer(0, v);  // begin
  }
  std::vector<uint8_t> data;
  data.reserve(length);
  while (data.size() < length) {
    auto block = transport_->read(EP_BULK_IN, CHUNK, 1000);
    if (!block) throw DeviceError("calibration upload stalled after " + std::to_string(data.size()) + " bytes");
    // Every package starts with its header length (0xd8) and "TI_CAL_METHOD".
    static const uint8_t head[] = {0xd8, 0, 0, 0, 'T', 'I', '_', 'C', 'A', 'L'};
    if (data.empty() &&
        (block->size() < sizeof head || !std::equal(head, head + sizeof head, block->begin())))
      throw DeviceError("calibration upload: the first " + std::to_string(block->size()) +
                        " bytes are not a calibration package (stale data from an interrupted stream?): " +
                        hex(std::vector<uint8_t>(block->begin(), block->begin() + std::min<size_t>(block->size(), 16))));
    uint32_t crc = uint32_t(crc32(0L, block->data(), uInt(block->size())));
    auto v = be32(crc);
    auto n = be32(uint32_t(block->size()));
    v.insert(v.end(), n.begin(), n.end());
    transfer(2, v);  // acknowledge the block
    data.insert(data.end(), block->begin(), block->end());
  }
  if (data.size() != length)
    throw DeviceError("calibration upload: got " + std::to_string(data.size()) + " bytes, expected " +
                      std::to_string(length));
  transfer(4, be32(uint32_t(crc32(0L, data.data(), uInt(data.size())))));  // end
  set_run_status(RUN_IDLE);
  LOG_INFO(LOG, "read %s calibration package: %u bytes", high ? "high" : "low", length);
  return data;
}

void Camera::start() {
  set_run_status(RUN_STREAM);
  // The first frame after (re)starting is the one the camera prepared when
  // streaming last stopped, possibly long ago (measured: always the previous
  // session's last frame id + 1, while the 16-bit counter keeps running at
  // 25 Hz and wraps every ~44 min).  Discard it and start the frame-order
  // check afresh.
  last_frame_id.reset();
  grab();
  last_frame_id.reset();
}

}  // namespace uti120
