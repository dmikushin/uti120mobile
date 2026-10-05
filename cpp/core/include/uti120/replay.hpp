// A Transport that plays back raw frames recorded from a real camera (the
// --raw output of the desktop tool), so a frontend can be exercised end to end
// without the device.  It answers the command protocol like the camera does:
// writes are echoed, reads return zeros, and frame requests are answered at
// 25 frames per second with recorded frames whose shutter flag matches the
// shutter state last commanded.  Frame counters are renumbered (and the CRC
// recomputed) so playback can loop.
#pragma once

#include <memory>
#include <string>

#include "uti120/device.hpp"

namespace uti120 {

// Throws DeviceError if the file holds no complete frames, or lacks frames
// with the shutter closed or open.
std::unique_ptr<Transport> open_replay(const std::string& raw_path);

}  // namespace uti120
