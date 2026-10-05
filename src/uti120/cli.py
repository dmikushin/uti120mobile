"""Command line interface: uti120 {info,snapshot,record,live}."""

import argparse
import contextlib
import logging
import shutil
import subprocess
import sys
import threading
import time

import numpy as np
from PIL import Image as PILImage

from . import palette, radiometry
from .device import Camera
from .frame import HEIGHT, WIDTH
from .process import AutoGain
from .stream import Stream

log = logging.getLogger("uti120")


def orient(img: np.ndarray, args) -> np.ndarray:
    if args.mirror:
        img = img[:, ::-1]
    if args.flip:
        img = img[::-1, :]
    return img


def render(signal: np.ndarray, gain: AutoGain, args) -> np.ndarray:
    return palette.colorize(gain(orient(signal, args)), args.palette)


def open_stream(args, raw_sink=None) -> Stream:
    cam = Camera()
    radiometer = None
    if getattr(args, "celsius", False):
        calibration, cam = radiometry.calibrated_camera(cam)
        radiometer = radiometry.Radiometer(calibration, radiometry.Settings(
            args.emissivity, args.reflected, args.distance, args.high_range))
    s = Stream(cam, dark_frames=args.dark_frames,
               recalibrate_s=args.recalibrate, raw_sink=raw_sink, radiometer=radiometer)
    s.start()
    return s


def summary(t: np.ndarray) -> dict:
    """min / max / mean / centre of a temperature map in sensor orientation."""
    return {"min": float(t.min()), "max": float(t.max()), "mean": float(t.mean()),
            "centre": float(t[HEIGHT // 2, WIDTH // 2])}


def cmd_info(args):
    with Camera() as cam:
        for k, v in cam.info().items():
            print(f"{k:12s} {v}")


def cmd_snapshot(args):
    s = open_stream(args)
    temps = []
    try:
        images = []
        while len(images) < args.average:
            im = s.read()
            images.append(im)
            if args.celsius and im.measurable:
                temps.append(s.radiometer.temperatures())
    finally:
        s.stop()
    if args.celsius:
        t = np.mean(temps, axis=0)
        print("temperature: " + ", ".join(f"{k} {v:.1f} C" for k, v in summary(t).items()))
        if args.npy:
            np.save(args.npy, t.astype(np.float32))
    signal = np.mean([im.signal for im in images], axis=0)
    rgb = render(signal, AutoGain(), args)
    out = PILImage.fromarray(rgb).resize((WIDTH * args.scale, HEIGHT * args.scale),
                                         PILImage.BICUBIC)
    out.save(args.output)
    if args.npy and not args.celsius:
        np.save(args.npy, signal)
    print(f"{args.output}: {images[-1].frame.describe()}, "
          f"signal p1/p50/p99 = {np.percentile(signal, [1, 50, 99]).round(1).tolist()}")


# Video is produced at a constant rate equal to the sensor's nominal 25 Hz:
# every output tick carries the newest calibrated frame (repeated if the camera
# was late), so the video duration always equals the wall-clock duration.
# ffmpeg's -use_wallclock_as_timestamps is not an alternative: with it ffmpeg
# stalls on the pipe (measured 0.9 fps delivered instead of 19).
VIDEO_FPS = 25


def ffmpeg_cmd(args, output: str) -> list[str]:
    w, h = WIDTH * args.scale, HEIGHT * args.scale
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", f"{WIDTH}x{HEIGHT}",
            "-framerate", str(VIDEO_FPS), "-i", "-",
            "-vf", f"scale={w}:{h}:flags=bicubic",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", output]


def ffplay_cmd(args) -> list[str]:
    w, h = WIDTH * args.scale, HEIGHT * args.scale
    return ["ffplay", "-hide_banner", "-loglevel", "error", "-autoexit", "-fflags", "nobuffer",
            "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", f"{WIDTH}x{HEIGHT}",
            "-framerate", str(VIDEO_FPS), "-window_title", "UTi120Mobile",
            "-vf", f"scale={w}:{h}:flags=bicubic", "-i", "-"]


class Capture(threading.Thread):
    """Reads the camera continuously and keeps only the newest image."""

    def __init__(self, stream: Stream):
        super().__init__(daemon=True)
        self.stream = stream
        self.cond = threading.Condition()
        self.latest = None
        self.count = 0
        self.error = None
        self.stopping = False

    def run(self):
        try:
            while not self.stopping:
                im = self.stream.read()
                with self.cond:
                    self.latest = im
                    self.count += 1
                    self.cond.notify_all()
        except Exception as e:
            with self.cond:
                self.error = e
                self.cond.notify_all()

    def newest(self):
        with self.cond:
            while self.latest is None and self.error is None:
                self.cond.wait()
            if self.error is not None:
                raise self.error
            return self.latest

    def stop(self):
        self.stopping = True
        self.join()


def pump(args, sink_cmd: list[str], duration: float):
    raw = open(args.raw, "wb") if args.raw else None
    try:
        s = open_stream(args, raw_sink=raw)
    except BaseException:
        if raw:
            raw.close()
        raise
    cap = Capture(s)
    cap.start()
    proc = subprocess.Popen(sink_cmd, stdin=subprocess.PIPE)
    gain = AutoGain()
    ticks, t0 = 0, time.monotonic()
    try:
        while not duration or ticks < duration * VIDEO_FPS:
            im = cap.newest()
            proc.stdin.write(np.ascontiguousarray(render(im.signal, gain, args)).tobytes())
            ticks += 1
            delay = t0 + ticks / VIDEO_FPS - time.monotonic()
            if delay > 0:
                time.sleep(delay)
    except (BrokenPipeError, KeyboardInterrupt):
        pass
    finally:
        cap.stop()
        s.stop()
        if raw:
            raw.close()
        try:
            proc.stdin.close()
        except BrokenPipeError:
            pass
        rc = proc.wait()
    dt = time.monotonic() - t0
    print(f"{ticks} video frames in {dt:.1f} s from {cap.count} camera frames "
          f"({cap.count / dt:.1f} fps), {sink_cmd[0]} exit code {rc}")
    return rc


def cmd_log(args):
    """Temperatures over time, one CSV row per interval: for watching a GPU under load."""
    args.celsius = True
    s = open_stream(args)
    with (open(args.output, "w") if args.output != "-" else contextlib.nullcontext(sys.stdout)) as out:
        try:
            log_rows(args, s, out)
        except KeyboardInterrupt:
            pass
        finally:
            s.stop()


def log_rows(args, s: Stream, out):
    out.write("time_s,min_c,max_c,mean_c,centre_c,camera_fpa_c\n")
    t0 = time.monotonic()
    next_at = t0
    while not args.duration or time.monotonic() - t0 < args.duration:
        im = s.read()
        if not im.measurable or time.monotonic() < next_at:
            continue
        row = summary(s.radiometer.temperatures())
        out.write(f"{time.monotonic() - t0:.2f},{row['min']:.2f},{row['max']:.2f},"
                  f"{row['mean']:.2f},{row['centre']:.2f},{im.frame.fpa_temp:.2f}\n")
        out.flush()
        next_at += args.interval


def require(tool: str):
    if shutil.which(tool) is None:
        sys.exit(f"{tool} not found in PATH")


def cmd_record(args):
    require("ffmpeg")
    sys.exit(pump(args, ffmpeg_cmd(args, args.output), args.duration))


def cmd_live(args):
    require("ffplay")
    pump(args, ffplay_cmd(args), args.duration)


def main(argv=None):
    p = argparse.ArgumentParser(prog="uti120", description="UNI-T UTi120Mobile thermal camera")
    p.add_argument("-v", "--verbose", action="count", default=0)
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("info", help="print device information").set_defaults(func=cmd_info)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--palette", choices=palette.NAMES, default="ironbow")
    common.add_argument("--scale", type=int, default=4, help="output upscaling factor")
    common.add_argument("--mirror", action="store_true", help="flip left-right")
    common.add_argument("--flip", action="store_true", help="flip upside-down")
    common.add_argument("--dark-frames", type=int, default=16,
                        help="closed-shutter frames averaged for offset calibration")
    common.add_argument("--recalibrate", type=float, default=0.0, metavar="SECONDS",
                        help="repeat the shutter calibration this often (0 = never)")

    measure = argparse.ArgumentParser(add_help=False)
    measure.add_argument("--emissivity", type=float, default=0.95, help="of the target (default 0.95)")
    measure.add_argument("--distance", type=float, default=0.6, help="to the target in metres (default 0.6)")
    measure.add_argument("--reflected", type=float, default=23.0,
                         help="reflected (ambient) temperature in C (default 23)")
    measure.add_argument("--high-range", action="store_true",
                         help="use the high measuring range (above ~120 C)")

    sp = sub.add_parser("snapshot", parents=[common, measure], help="save one image")
    sp.add_argument("-o", "--output", default="uti120.png")
    sp.add_argument("--average", type=int, default=8, help="frames averaged into the image")
    sp.add_argument("--celsius", action="store_true",
                    help="also measure temperatures (the first time, reads and caches the "
                         "camera's calibration, which restarts the camera)")
    sp.add_argument("--npy", help="also save the signal (with --celsius: temperatures in C) as .npy")
    sp.set_defaults(func=cmd_snapshot)

    sp = sub.add_parser("log", parents=[common, measure],
                        help="log min/max/mean/centre temperatures over time as CSV")
    sp.add_argument("-o", "--output", default="-", help="CSV file (default: standard output)")
    sp.add_argument("-t", "--duration", type=float, default=0.0, help="seconds (0 = until interrupted)")
    sp.add_argument("--interval", type=float, default=1.0, help="seconds between rows (default 1)")
    sp.set_defaults(func=cmd_log)

    for name, func, helptext in (("record", cmd_record, "record a video with ffmpeg"),
                                 ("live", cmd_live, "show live video with ffplay")):
        sp = sub.add_parser(name, parents=[common], help=helptext)
        if name == "record":
            sp.add_argument("-o", "--output", default="uti120.mp4")
        sp.add_argument("-t", "--duration", type=float, default=10.0 if name == "record" else 0.0,
                        help="seconds (0 = until interrupted)")
        sp.add_argument("--raw", help="also write every raw 25600-byte frame to this file")
        sp.set_defaults(func=func)

    args = p.parse_args(argv)
    logging.basicConfig(level=logging.WARNING - 10 * args.verbose,
                        format="%(asctime)s %(name)s %(levelname)s: %(message)s")
    args.func(args)


if __name__ == "__main__":
    main()
