# uti120

Driver, video tools and an Android app for the **UNI-T UTi120Mobile** USB
thermal camera (120×90 microbolometer, USB ID `5656:1201`). This is an
independent open-source implementation; all forms share the same protocol
handling and image processing:

* **Python** (`src/`) — a small library and CLI on pyusb; video goes through
  external `ffmpeg`/`ffplay` processes.
* **C++** (`cpp/`) — a single self-contained executable: USB, calibration,
  H.264/MP4 encoding, PNG and the live window all run in one process, with
  libusb, FFmpeg, x264 and SDL3 linked in statically.
* **Android** (`android/`) — a Kotlin/Jetpack Compose app on the same C++
  backend (`cpp/core`), with the vendor app's screen layout: live image, tap
  for a photo and hold for video, palettes, tap the image to recalibrate.

All of them measure temperatures in °C with the camera's own calibration.

## Example

<img src="docs/screenshot-android.jpg" width="360" alt="The Android app showing a thermal image of a PC motherboard">

The Android app on a phone, looking at a running PC motherboard (ironbow
palette; brighter is warmer).

## Install

### C++ (single executable)

```sh
cmake -S cpp -B cpp/build -G Ninja
cmake --build cpp/build          # also downloads and builds the dependencies
cpp/build/uti120 info
```

Build requirements: CMake ≥ 3.20, a C++20 compiler, `make`, `nasm` (for x264)
and network access on the first build. The pinned sources of zlib 1.3.1,
libusb 1.0.29, x264, FFmpeg 8.0 (configured with only the MP4 muxer and the
libx264 and PNG encoders) and SDL3 3.2.24 are verified by SHA-256 and built as
static libraries into `cpp/build/deps`. The resulting executable needs only
libc and libm; for `live`, SDL3 loads the X11 or Wayland client library of the
running desktop at run time (have their development headers installed when
building, or the corresponding SDL3 backend is left out).

`ctest --test-dir cpp/build` runs the offline tests.

The C++ code is split into the backend `cpp/core` (protocol, calibration,
processing, rendering; a static library with no media dependencies) and the
desktop frontend `cpp/desktop` (command line, FFmpeg/x264 encoding, SDL3 window).

### Android

```sh
cd android
echo "sdk.dir=$HOME/Android/Sdk" > local.properties
./gradlew assembleDebug       # app/build/outputs/apk/debug/app-debug.apk
```

Requires the Android SDK with platform 37.2, NDK 28.2.13676358 and CMake
3.31.6 (`sdkmanager "platforms;android-37.2" "ndk;28.2.13676358" "cmake;3.31.6"`).
libusb is downloaded at its pinned, hash-checked release and built with the
NDK; the backend comes from `cpp/core`. Plug the camera into the phone (USB
OTG), allow access when asked. Tap the capture button for a photo
(`Pictures/UTi120`); press and hold it to record video until you let go
(`Movies/UTi120`).

Debug builds can run without the camera on raw frames recorded with
`uti120 record --raw`:

```sh
adb push frames.raw /data/local/tmp/replay.raw
adb shell run-as io.github.dmikushin.uti120 sh -c "'mkdir -p files && cp /data/local/tmp/replay.raw files/'"
adb shell am start -n io.github.dmikushin.uti120/.MainActivity -e replay /data/data/io.github.dmikushin.uti120/files/replay.raw
```

### Python

```sh
sudo pacman -S python-pyusb python-numpy python-pillow ffmpeg   # Arch; or pip deps
pipx install .            # or: pip install .
sudo cp udev/70-uni-t-uti120mobile.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

The udev rule gives the `wheel` group access to the camera; edit `GROUP` to suit.

## Use

```sh
uti120 info                                  # firmware, sensor id, status
uti120 snapshot -o board.png                 # one image (8 frames averaged)
uti120 record -t 30 -o board.mp4             # 30 s video, 25 fps, 480x360
uti120 record -t 30 --raw board.raw          # ...plus every raw frame, lossless
uti120 live                                  # live window (Esc or q closes it)
```

Common options: `--palette {grey,ironbow,rainbow}`, `--scale N` (upscaling,
default 4), `--mirror`, `--flip`, `--recalibrate SECONDS` (repeat the shutter
calibration periodically during long recordings), `-v` (log; `-vv` protocol
debug).

The image is relative: brighter means warmer, scaled automatically between the
1st and 99th percentile.

### Temperatures

```sh
uti120 snapshot --celsius -o board.png       # also prints min/max/mean/centre in C
uti120 snapshot --celsius --npy board.npy    # ...and saves the 120x90 map in C
uti120 live --celsius                        # min/max/centre in the window title (C++ tool)
uti120 log -t 600 --interval 1 -o gpu.csv    # CSV time series: a GPU under load
```

`--emissivity E` (default 0.95), `--distance M` (0.6), `--reflected C` (23) and
`--high-range` (for targets above ~120 °C) set the measuring conditions.

The first `--celsius` run reads the camera's own calibration (about 0.5 MB from
its flash plus eight correction coefficients) and caches it in
`~/.cache/uti120/<sensor id>`. The camera's firmware stops sending frames after
that read until it restarts, so the tool restarts the camera and waits for it;
later runs use the cache.

## How it works

Protocol recovered from the vendor Android app (`com.unit.usblib.armlib`) and
verified against the device:

| What | Where |
|---|---|
| commands | interrupt OUT `0x01` → reply on interrupt IN `0x81`; `[func, reg, count, values(BE32)...]` |
| register read / write | func `0x05` / `0x04` (system), `0x0B` / `0x0A` (sensor) |
| start streaming | write system register `0xF0` (run status) = 2 |
| request a frame | write single byte `0x81`, read 25600 bytes from bulk IN `0x82` |
| shutter | sensor register 3: 1 = closed, 0 = open |
| on-camera NUC | sensor register 4 = 1 |

Frame layout (little-endian 16-bit words): one header row of 120 words (magic
`0xAA55`, frame counter, geometry 120×92, shutter/tube/FPA temperatures in
0.01 °C, shutter and NUC status), then 92 sensor rows of which the first two are
reference rows and 90 are image, then zero padding, and a CRC-32 of the frame in
the last 4 bytes.

The raw image is dominated by per-pixel offsets. At start-up the program asks the
camera for an NUC, closes the shutter, averages 16 frames as the offset map,
opens the shutter and subtracts that map from every frame; outlier pixels are
replaced by the median of their neighbours.

Temperatures are computed exactly as the vendor app computes them, from the
calibration stored in each camera:

1. **Raw frame → Y16.** Offset against the last shutter frame times a per-pixel
   gain chosen by the sensor temperature (7 gain tables per range), bad-pixel
   replacement, column and row stripe removal (range-weighted 9-tap filters),
   a 3×3 Gaussian.
2. **Y16 → °C.** Counts-to-temperature curves for 7 sensor temperatures and two
   calibration distances, interpolated between them; corrected for the drift of
   the lens temperature since the last shutter; then for emissivity and
   reflected temperature through a radiance table.
3. **Band correction.** A linear `k·t + b` per temperature band from the camera's
   registers 49..56.

Steps 1 and 2 are reimplementations, written from the decompiled library (Python: `src/uti120/vendor_y16.py`,
`vendor_temp.py`; C++: `cpp/core/src/radiometry.cpp`) that match the vendor's
native library bit for bit: the tests compare them with outputs of that
library run on recorded and synthetic frames (`tests/data/radiometry`). The
only data taken from the vendor's library is the 16384-entry radiance table
used for the emissivity correction (`src/uti120/data/emiss_curve.bin`); it is
close to, but not exactly, a Planck integral over 8–13 µm (up to ~3 °C apart
at room temperature), and the exact table is needed to match the vendor.

The accuracy is therefore the vendor's: this is the same computation on the
same calibration, not an independent measurement.

## Tests

Both implementations are tested offline on frames recorded from a real camera
(`tests/data`):

```sh
python3 -m pytest tests
ctest --test-dir cpp/build
```

## License

The source code in this repository is MIT-licensed. The C++ executable links
x264 and an FFmpeg build configured with `--enable-gpl`, so the executable as a
whole is distributed under the GNU GPL version 2 or later. The Android app
links libusb statically, which is under the GNU LGPL 2.1.
