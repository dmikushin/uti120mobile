# uti120

Linux driver and video tools for the **UNI-T UTi120Mobile** USB thermal camera
(120×90 microbolometer, USB ID `5656:1201`). The vendor only supports Android;
this is an independent open-source implementation in Python on top of libusb.

![motherboard](docs/snapshot.png)

## Install

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
uti120 live                                  # live window via ffplay
```

Common options: `--palette {grey,ironbow,rainbow}`, `--scale N` (upscaling,
default 4), `--mirror`, `--flip`, `--recalibrate SECONDS` (repeat the shutter
calibration periodically during long recordings), `--no-nuc`, `-v` (log).

The image is relative: brighter means warmer, scaled automatically between the
1st and 99th percentile. Radiometric temperatures are not implemented.

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

## Tests

```sh
python3 -m pytest tests     # offline, on frames recorded from a real camera
```

## License

MIT
