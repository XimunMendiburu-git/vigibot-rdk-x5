#!/usr/bin/env python3
"""
Vigibot RDK X5 — raw camera encoder.

Modes (env):
  VIGI_HW_ENCODE=1  Camera NV12 -> Wave521 hb_mm_mc Baseline -> TCP 8043
  default           Camera NV12 -> libx264 Baseline -> TCP 8043
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
HW_ENCODER = "/usr/local/vigiclient/vigi-hbn-encode"
HW_INI = "/tmp/vigi_hw_encode.ini"
HW_OUT = "/tmp/vigi_hw_live.h264"

camera = None
sock = None
ffmpeg_proc = None
hw_proc = None
reader_thread = None
stop_reader = threading.Event()
running = True


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def hw_mode() -> bool:
    return os.environ.get("VIGI_HW_ENCODE", "").strip() in ("1", "true", "yes")


def cleanup() -> None:
    global camera, sock, ffmpeg_proc, hw_proc, reader_thread
    stop_reader.set()
    running = False
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
    if hw_proc is not None:
        try:
            if hw_proc.stdin:
                hw_proc.stdin.close()
        except OSError:
            pass
        try:
            hw_proc.terminate()
            hw_proc.wait(timeout=2)
        except Exception:
            try:
                hw_proc.kill()
            except Exception:
                pass
        hw_proc = None
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


def clamp_fps_bitrate(fps: int, bitrate: int) -> tuple[int, int]:
    # IMX219 on this board only enumerates a 15fps sensor mode.
    fps = 15
    bitrate = min(max(int(bitrate), 300000), 500000)
    return fps, bitrate


def write_hw_ini(fps: int, bitrate_kbps: int) -> None:
    content = """[encode]
encode_streams = 0x01

[venc_stream1]
codec_type = 0
width = %d
height = %d
frame_rate = %d
bit_rate = %d
input = /dev/stdin
output = %s
frame_num = 900000000
profile = h264_constrained_baseline@L5_2
external_buffer = 1
""" % (CAP_WIDTH, CAP_HEIGHT, fps, bitrate_kbps, HW_OUT)
    with open(HW_INI, "w", encoding="utf-8") as handle:
        handle.write(content)


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


def start_hw_encoder(fps: int, bitrate_kbps: int) -> subprocess.Popen:
    try:
        os.remove(HW_OUT)
    except FileNotFoundError:
        pass
    write_hw_ini(fps, bitrate_kbps)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = "/usr/hobot/lib:/usr/lib:" + env.get("LD_LIBRARY_PATH", "")
    return subprocess.Popen(
        [HW_ENCODER, "-f", HW_INI, "-e", "0x1"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        bufsize=0,
        env=env,
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


def start_hw_file_reader(client: socket.socket) -> threading.Thread:
    def _run() -> None:
        offset = 0
        while not stop_reader.is_set():
            try:
                if not os.path.exists(HW_OUT):
                    time.sleep(0.02)
                    continue
                size = os.path.getsize(HW_OUT)
                if size < offset:
                    offset = 0
                if size <= offset:
                    time.sleep(0.005)
                    continue
                with open(HW_OUT, "rb") as handle:
                    handle.seek(offset)
                    chunk = handle.read(65536)
                if chunk:
                    client.sendall(chunk)
                    offset += len(chunk)
                else:
                    time.sleep(0.005)
            except (BrokenPipeError, ConnectionResetError, OSError):
                break

    thread = threading.Thread(target=_run, daemon=True)
    thread.start()
    return thread


def connect_video_sink(host: str, port: int) -> socket.socket:
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    while running:
        try:
            client.connect((host, port))
            return client
        except (ConnectionRefusedError, OSError):
            time.sleep(0.5)
    raise RuntimeError("video sink connect aborted")


def capture_frame():
    global camera
    for _ in range(WARMUP_ATTEMPTS):
        frame = camera.get_img(2, CAP_WIDTH, CAP_HEIGHT)
        if frame is not None:
            return frame
        time.sleep(FRAME_WAIT_S)
    return None


def pump_camera_to(process: subprocess.Popen, fps: int, label: str) -> int:
    global camera
    expected = CAP_WIDTH * CAP_HEIGHT * 3 // 2
    sent_frames = 0
    interval = 1.0 / fps

    while running:
        t0 = time.time()
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
            process.stdin.write(frame[:expected])
            process.stdin.flush()
        except BrokenPipeError:
            log("%s stdin closed" % label)
            return 1

        sent_frames += 1
        if sent_frames == 1 or sent_frames % 30 == 0:
            log("sent %d nv12 frames to %s" % (sent_frames, label))

        dt = time.time() - t0
        if interval > dt:
            time.sleep(interval - dt)

    return 0


def run_sw_encode(fps: int, bitrate: int) -> int:
    global ffmpeg_proc, reader_thread, sock
    ffmpeg_proc = start_ffmpeg_nv12(CAP_WIDTH, CAP_HEIGHT, fps, bitrate)
    reader_thread = start_socket_reader(sock, ffmpeg_proc.stdout)
    log("video format: NV12 -> libx264 baseline (low-latency)")
    return pump_camera_to(ffmpeg_proc, fps, "x264")


def run_hw_encode(fps: int, bitrate: int) -> int:
    global hw_proc, reader_thread, sock
    bitrate_kbps = max(int(bitrate) // 1000, 300)
    hw_proc = start_hw_encoder(fps, bitrate_kbps)
    reader_thread = start_hw_file_reader(sock)
    log(
        "video format: NV12 -> Wave521 hb_mm_mc baseline@%dfps %dkbps"
        % (fps, bitrate_kbps)
    )
    return pump_camera_to(hw_proc, fps, "hw")


def main() -> int:
    global camera, sock, running

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)
    os.environ.setdefault("LD_LIBRARY_PATH", "/usr/hobot/lib:/usr/lib")

    if len(sys.argv) < 4:
        log("usage: vigi-encode-rdk.py WIDTH HEIGHT FPS [BITRATE]")
        return 1

    req_w = int(sys.argv[1])
    req_h = int(sys.argv[2])
    fps, bitrate = clamp_fps_bitrate(int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 600000)
    use_hw = hw_mode()

    log(
        "starting %s: vigibot %dx%d@req -> native %dx%d@%d br=%d"
        % ("HW baseline" if use_hw else "SW x264", req_w, req_h, CAP_WIDTH, CAP_HEIGHT, fps, bitrate)
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
    try:
        sock = connect_video_sink("127.0.0.1", VIDEO_PORT)
    except RuntimeError:
        cleanup()
        return 1
    log("connected to tcp://127.0.0.1:%d" % VIDEO_PORT)

    try:
        if use_hw and os.path.isfile(HW_ENCODER):
            return run_hw_encode(fps, bitrate)
        if use_hw:
            log("HW encoder missing, falling back to libx264")
        return run_sw_encode(fps, bitrate)
    except (BrokenPipeError, ConnectionResetError):
        log("video sink disconnected")
        return 0
    finally:
        cleanup()

    return 0


if __name__ == "__main__":
    sys.exit(main())
