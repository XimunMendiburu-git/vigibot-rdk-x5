#!/usr/bin/env python3
"""
Vigibot RDK X5 — YOLO overlay:
  Camera NV12 → YOLOv5s_v7 BPU → draw → libx264 Baseline → TCP 8043

Stream-first: passthrough NV12 frames before model load. See docs/yolo-source.md.
"""

from __future__ import annotations

import ctypes
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time

import cv2
import numpy as np
from hobot_dnn import pyeasy_dnn as dnn

try:
    from hobot_vio import libsrcampy
except ImportError:
    from hobot_vio_rdkx5 import libsrcampy

VIDEO_PORT = 8043
CAP_WIDTH = 640
CAP_HEIGHT = 480
MODEL_SIZE = 640
NV12_SIZE = CAP_WIDTH * CAP_HEIGHT * 3 // 2
WARMUP_ATTEMPTS = 200
FRAME_WAIT_S = 0.02
MAX_FPS = 15
MODEL_PATH = "/opt/hobot/model/x5/basic/yolov5s_v7_640x640_nv12.bin"
POSTPROCESS_SO = "/usr/lib/libpostprocess.so"
SCORE_TH = 0.4
NMS_TH = 0.45
NMS_TOP_K = 20
PASSTHROUGH_FRAMES = 30
INFER_EVERY = int(os.environ.get("VIGI_INFER_EVERY", "2"))

camera = None
sock = None
ffmpeg_proc = None
reader_thread = None
models = None
post = None
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


def nv12_to_bgr(nv12: bytes) -> np.ndarray:
    yuv = np.frombuffer(nv12[:NV12_SIZE], dtype=np.uint8).reshape((CAP_HEIGHT * 3 // 2, CAP_WIDTH))
    return cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_NV12)


def bgr_to_nv12(bgr: np.ndarray) -> bytes:
    yuv = cv2.cvtColor(bgr, cv2.COLOR_BGR2YUV_I420)
    h, w = CAP_HEIGHT, CAP_WIDTH
    y = yuv[0:h, :]
    u = yuv[h : h + h // 4, :].reshape(h // 2, w // 2)
    v = yuv[h + h // 4 :, :].reshape(h // 2, w // 2)
    uv = np.empty((h // 2, w), dtype=np.uint8)
    uv[:, 0::2] = u
    uv[:, 1::2] = v
    return y.tobytes() + uv.tobytes()


def load_postprocess() -> ctypes.CDLL:
    lib = ctypes.CDLL(POSTPROCESS_SO)
    lib.Yolov5PostProcess.argtypes = [
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
    ]
    lib.Yolov5PostProcess.restype = ctypes.c_char_p
    return lib


def run_inference(bgr640: np.ndarray) -> list[dict]:
    global models, post
    if models is None or post is None:
        return []
    nv12_in = cv2.cvtColor(bgr640, cv2.COLOR_BGR2YUV_I420)
    h = w = MODEL_SIZE
    y = nv12_in[0:h, :]
    u = nv12_in[h : h + h // 4, :].reshape(h // 2, w // 2)
    v = nv12_in[h + h // 4 :, :].reshape(h // 2, w // 2)
    uv = np.empty((h // 2, w), dtype=np.uint8)
    uv[:, 0::2] = u
    uv[:, 1::2] = v
    tensor = y.tobytes() + uv.tobytes()
    outputs = models[0].forward(tensor)
    if not outputs:
        return []
    result_str = outputs[0].buffer.tobytes()
    raw = post.Yolov5PostProcess(
        result_str,
        len(result_str),
        ctypes.c_float(SCORE_TH),
        ctypes.c_float(NMS_TH),
        NMS_TOP_K,
        0,
    )
    if not raw:
        return []
    text = raw.decode("utf-8", errors="ignore")
    if len(text) > 16:
        text = text[16:]
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return []
    return data if isinstance(data, list) else []


def draw_detections(bgr: np.ndarray, detections: list[dict]) -> np.ndarray:
    out = bgr.copy()
    for det in detections:
        try:
            name = det.get("name", "?")
            score = float(det.get("score", 0))
            box = det.get("bbox", det.get("box", []))
            if len(box) < 4:
                continue
            x1, y1, x2, y2 = [int(v) for v in box[:4]]
            cv2.rectangle(out, (x1, y1), (x2, y2), (0, 255, 0), 2)
            cv2.putText(
                out,
                f"{name} {score:.2f}",
                (x1, max(0, y1 - 5)),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.5,
                (0, 255, 0),
                1,
            )
        except (TypeError, ValueError):
            continue
    return out


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
        except OSError as exc:
            log(f"tcp send failed: {exc}")
            break


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


def load_model() -> None:
    global models, post
    log(f"loading model {MODEL_PATH}")
    models = dnn.load(MODEL_PATH)
    post = load_postprocess()
    log("yolo model ready")


def write_nv12(nv12: bytes) -> bool:
    global ffmpeg_proc
    if not ffmpeg_proc or not ffmpeg_proc.stdin:
        return False
    try:
        ffmpeg_proc.stdin.write(nv12[:NV12_SIZE])
        ffmpeg_proc.stdin.flush()
        return True
    except BrokenPipeError:
        return False


def capture_loop(fps: int) -> None:
    global camera, stop, models
    frame_interval = 1.0 / max(fps, 1)
    frames = 0
    while not stop and camera:
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
        nv12 = bytes(img[:NV12_SIZE])
        frames += 1

        if frames <= PASSTHROUGH_FRAMES:
            if not write_nv12(nv12):
                break
            if frames == PASSTHROUGH_FRAMES:
                load_model()
            if frames == 1 or frames % 30 == 0:
                log(f"sent {frames} passthrough nv12 frames")
        else:
            try:
                bgr = nv12_to_bgr(nv12)
                if models and frames % INFER_EVERY == 0:
                    resized = cv2.resize(bgr, (MODEL_SIZE, MODEL_SIZE))
                    dets = run_inference(resized)
                    bgr = draw_detections(bgr, dets)
                out_nv12 = bgr_to_nv12(bgr)
                if not write_nv12(out_nv12):
                    break
                if frames % 150 == 0:
                    log(f"sent {frames} yolo frames")
            except Exception as exc:
                log(f"yolo loop error: {exc}")
                if not write_nv12(nv12):
                    break

        elapsed = time.perf_counter() - t0
        sleep_s = frame_interval - elapsed
        if sleep_s > 0:
            time.sleep(sleep_s)


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
    log(
        f"yolo video: {CAP_WIDTH}x{CAP_HEIGHT} passthrough={PASSTHROUGH_FRAMES} "
        f"infer_every={INFER_EVERY} @{fps}fps"
    )
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
