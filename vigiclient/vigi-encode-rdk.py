#!/usr/bin/env python3
"""
Vigibot RDK X5 — low latency path:
  Camera NV12 → libx264 Baseline → TCP 8043
(no Hobot H.264 encoder)
"""

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
WARMUP_ATTEMPTS = 200
FRAME_WAIT_S = 0.02

camera = None
sock = None
ffmpeg_proc = None
reader_thread = None
stop_reader = threading.Event()


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def cleanup() -> None:
    global camera, sock, ffmpeg_proc, reader_thread
    stop_reader.set()
    if ffmpeg_proc is not None:
        try:
            if ffmpeg_proc.stdin:
                ffmpeg_proc.stdin.close()
        except OSError:
            pass
        try:
            ffmpeg_proc.terminate()
            ffmpeg_proc.wait(timeout=2)
        except Exception:
            try:
                ffmpeg_proc.kill()
            except Exception:
                pass
        ffmpeg_proc = None
    if sock is not None:
        try:
            sock.close()
        except OSError:
            pass
        sock = None
    if camera is not None:
        try:
            camera.close_cam()
        except Exception:
            pass
        camera = None
    reader_thread = None


def handle_signal(_signum, _frame) -> None:
    cleanup()
    sys.exit(0)


def start_ffmpeg_nv12(width: int, height: int, fps: int, bitrate: int) -> subprocess.Popen:
    bps = max(int(bitrate), 300000)
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel", "error",
        "-fflags", "nobuffer",
        "-flags", "low_delay",
        "-f", "rawvideo",
        "-pix_fmt", "nv12",
        "-s:v", "%dx%d" % (width, height),
        "-r", str(fps),
        "-i", "pipe:0",
        "-an",
        "-c:v", "libx264",
        "-profile:v", "baseline",
        "-level", "3.1",
        "-pix_fmt", "yuv420p",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-b:v", str(bps),
        "-maxrate", str(bps),
        "-bufsize", str(bps),
        "-g", str(fps),
        "-keyint_min", str(fps),
        "-bf", "0",
        "-sc_threshold", "0",
        "-threads", "2",
        "-x264-params", "repeat-headers=1:annexb=1:sliced-threads=0",
        "-flush_packets", "1",
        "-muxdelay", "0",
        "-muxpreload", "0",
        "-f", "h264",
        "pipe:1",
    ]
    return subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )


def start_socket_reader(client: socket.socket, source) -> threading.Thread:
    def _run() -> None:
        try:
            while not stop_reader.is_set():
                chunk = source.read(65536)
                if not chunk:
                    break
                client.sendall(chunk)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    thread = threading.Thread(target=_run, daemon=True)
    thread.start()
    return thread


def connect_video_sink(host: str, port: int) -> socket.socket:
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    while True:
        try:
            client.connect((host, port))
            return client
        except (ConnectionRefusedError, OSError):
            time.sleep(0.5)


def capture_frame():
    global camera
    for _ in range(WARMUP_ATTEMPTS):
        frame = camera.get_img(2, CAP_WIDTH, CAP_HEIGHT)
        if frame is not None:
            return frame
        time.sleep(FRAME_WAIT_S)
    return None


def main() -> int:
    global camera, sock, ffmpeg_proc, reader_thread

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    if len(sys.argv) < 4:
        log("usage: vigi-encode-rdk.py WIDTH HEIGHT FPS [BITRATE]")
        return 1

    req_w = int(sys.argv[1])
    req_h = int(sys.argv[2])
    fps = int(sys.argv[3])
    _bitrate = int(sys.argv[4]) if len(sys.argv) > 4 else 600000

    # Plafonds latence / CPU X5
    fps = min(max(fps, 5), 15)
    _bitrate = min(max(_bitrate, 300000), 700000)

    expected = CAP_WIDTH * CAP_HEIGHT * 3 // 2

    log(
        "starting NV12->x264: vigibot %dx%d@req -> native %dx%d@%d br=%d"
        % (req_w, req_h, CAP_WIDTH, CAP_HEIGHT, fps, _bitrate)
    )

    camera = libsrcampy.Camera()
    if camera.open_cam(0, -1, fps, CAP_WIDTH, CAP_HEIGHT) != 0:
        log("camera.open_cam failed")
        cleanup()
        return 1

    if capture_frame() is None:
        log("camera warmup failed")
        cleanup()
        return 1

    log("camera ready, connecting to vigiclient")
    sock = connect_video_sink("127.0.0.1", VIDEO_PORT)
    log("connected to tcp://127.0.0.1:%d" % VIDEO_PORT)

    try:
        ffmpeg_proc = start_ffmpeg_nv12(CAP_WIDTH, CAP_HEIGHT, fps, _bitrate)
        reader_thread = start_socket_reader(sock, ffmpeg_proc.stdout)
        log("video format: NV12 -> libx264 baseline (low-latency)")
    except Exception as exc:
        log("ffmpeg start failed: %s" % exc)
        cleanup()
        return 1

    sent_frames = 0

    try:
        while True:
            # Toujours prendre la frame la plus récente (jette le retard caméra)
            frame = camera.get_img(2, CAP_WIDTH, CAP_HEIGHT)
            for _ in range(3):
                newer = camera.get_img(2, CAP_WIDTH, CAP_HEIGHT)
                if newer is None:
                    break
                frame = newer

            if frame is None or len(frame) < expected:
                time.sleep(FRAME_WAIT_S)
                continue

            try:
                ffmpeg_proc.stdin.write(frame[:expected])
                ffmpeg_proc.stdin.flush()
            except BrokenPipeError:
                log("ffmpeg stdin closed")
                return 1

            sent_frames += 1
            if sent_frames == 1 or sent_frames % 30 == 0:
                log("sent %d nv12 frames to x264" % sent_frames)

            # Petite pause pour ne pas saturer le CPU
            time.sleep(0.001)

    except (BrokenPipeError, ConnectionResetError):
        log("video sink disconnected")
        return 0
    finally:
        cleanup()

    return 0


if __name__ == "__main__":
    sys.exit(main())
