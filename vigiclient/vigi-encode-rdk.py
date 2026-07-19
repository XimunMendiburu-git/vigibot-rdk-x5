#!/usr/bin/env python3
"""
Vigibot RDK X5 — camera NV12 → libx264 Baseline → TCP 127.0.0.1:8043

Software encoder path (VIGI_USE_FFMPEG=1). Hardware Wave521 path abandoned for
browser/Vigibot compatibility — see docs/video-encoding.md.
"""

from __future__ import annotations

import os
import signal
import socket
import subprocess
import sys
import threading
import time

try:
    from hobot_vio import libsrcampy
except ImportError:
    from hobot_vio_rdkx5 import libsrcampy

VIDEO_PORT = 8043
CAP_WIDTH = 640
CAP_HEIGHT = 480
NV12_SIZE = CAP_WIDTH * CAP_HEIGHT * 3 // 2
WARMUP_ATTEMPTS = 200
FRAME_WAIT_S = 0.02
MAX_FPS = 15

camera = None
sock = None
ffmpeg_proc = None
reader_thread = None
stop = False


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def parse_args(argv: list[str]) -> tuple[int, int, int, int]:
    if len(argv) < 5:
        log(f"usage: {argv[0]} WIDTH HEIGHT FPS BITRATE")
        sys.exit(2)
    width = int(argv[1])
    height = int(argv[2])
    fps = min(int(argv[3]), MAX_FPS)
    bitrate = int(argv[4])
    bitrate = max(300_000, min(bitrate, 700_000))
    return width, height, fps, bitrate


def start_ffmpeg(fps: int, bitrate: int) -> subprocess.Popen:
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "nv12",
        "-s",
        f"{CAP_WIDTH}x{CAP_HEIGHT}",
        "-r",
        str(fps),
        "-i",
        "pipe:0",
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-profile:v",
        "baseline",
        "-level",
        "3.1",
        "-b:v",
        str(bitrate),
        "-maxrate",
        str(bitrate),
        "-bufsize",
        str(bitrate // 2),
        "-x264-params",
        "repeat-headers=1:annexb=1:sliced-threads=0",
        "-f",
        "h264",
        "pipe:1",
    ]
    return subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def reader_loop() -> None:
    global sock, ffmpeg_proc, stop
    sent = 0
    while not stop and ffmpeg_proc and ffmpeg_proc.stdout and sock:
        chunk = ffmpeg_proc.stdout.read(65536)
        if not chunk:
            break
        try:
            sock.sendall(chunk)
            sent += len(chunk)
            if sent and sent % (256 * 1024) < len(chunk):
                log(f"sent {sent // 1024} KiB h264")
        except OSError as exc:
            log(f"tcp send failed: {exc}")
            break


def open_camera(fps: int) -> None:
    global camera
    camera = libsrcampy.Camera()
    for attempt in range(WARMUP_ATTEMPTS):
        if stop:
            return
        try:
            camera.open_cam(0, -1, fps, CAP_WIDTH, CAP_HEIGHT)
            log(f"camera ready {CAP_WIDTH}x{CAP_HEIGHT}@{fps}")
            return
        except Exception as exc:
            if attempt == 0 or attempt % 20 == 0:
                log(f"open_cam attempt {attempt + 1}: {exc}")
            time.sleep(FRAME_WAIT_S)
    raise RuntimeError("camera.open_cam failed")


def capture_loop(fps: int) -> None:
    global camera, ffmpeg_proc, stop
    frame_interval = 1.0 / max(fps, 1)
    frames = 0
    while not stop and camera and ffmpeg_proc and ffmpeg_proc.stdin:
        t0 = time.perf_counter()
        try:
            img = camera.get_img(2, CAP_WIDTH, CAP_HEIGHT)
        except Exception as exc:
            log(f"get_img failed: {exc}")
            time.sleep(FRAME_WAIT_S)
            continue
        if not img or len(img) < NV12_SIZE:
            time.sleep(FRAME_WAIT_S)
            continue
        try:
            ffmpeg_proc.stdin.write(bytes(img[:NV12_SIZE]))
            ffmpeg_proc.stdin.flush()
        except BrokenPipeError:
            log("ffmpeg stdin closed")
            break
        frames += 1
        if frames == 1 or frames % 150 == 0:
            log(f"sent {frames} nv12 frames")
        elapsed = time.perf_counter() - t0
        sleep_s = frame_interval - elapsed
        if sleep_s > 0:
            time.sleep(sleep_s)


def connect_tcp() -> None:
    global sock
    deadline = time.time() + 30
    while not stop and time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", VIDEO_PORT), timeout=2)
            sock = s
            log(f"connected tcp://127.0.0.1:{VIDEO_PORT}")
            return
        except OSError:
            time.sleep(0.5)
    raise RuntimeError(f"cannot connect to tcp 127.0.0.1:{VIDEO_PORT}")


def cleanup(*_args) -> None:
    global stop, camera, sock, ffmpeg_proc, reader_thread
    stop = True
    if ffmpeg_proc and ffmpeg_proc.stdin:
        try:
            ffmpeg_proc.stdin.close()
        except OSError:
            pass
    if ffmpeg_proc:
        try:
            ffmpeg_proc.terminate()
            ffmpeg_proc.wait(timeout=2)
        except Exception:
            try:
                ffmpeg_proc.kill()
            except Exception:
                pass
    if reader_thread and reader_thread.is_alive():
        reader_thread.join(timeout=2)
    if sock:
        try:
            sock.close()
        except OSError:
            pass
    if camera:
        try:
            camera.close_cam()
        except Exception:
            pass


def main() -> None:
    global ffmpeg_proc, reader_thread
    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)

    _w, _h, fps, bitrate = parse_args(sys.argv)
    use_ffmpeg = os.environ.get("VIGI_USE_FFMPEG", "1") != "0"
    if not use_ffmpeg:
        log("VIGI_USE_FFMPEG=0: HW encoder path not supported in this POC build")
        sys.exit(1)

    log(f"video format: {CAP_WIDTH}x{CAP_HEIGHT} nv12 libx264 @{fps}fps ~{bitrate}bps")
    connect_tcp()
    ffmpeg_proc = start_ffmpeg(fps, bitrate)
    reader_thread = threading.Thread(target=reader_loop, daemon=True)
    reader_thread.start()
    open_camera(fps)
    capture_loop(fps)


if __name__ == "__main__":
    try:
        main()
    finally:
        cleanup()
