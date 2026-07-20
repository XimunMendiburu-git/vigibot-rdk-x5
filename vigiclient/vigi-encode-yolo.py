#!/usr/bin/env python3
"""
Vigibot RDK X5 — YOLO overlay (stream-first):
  Camera NV12 → (warmup frames) → YOLOv5s_v7 draw → libx264 → TCP 8043
"""
import ctypes
import json
import signal
import socket
import subprocess
import sys
import threading
import time

import cv2
import numpy as np

try:
    from hobot_vio import libsrcampy
except ImportError:
    from hobot_vio_rdkx5 import libsrcampy

VIDEO_PORT = 8043
CAP_WIDTH = 640
CAP_HEIGHT = 480
WARMUP_ATTEMPTS = 200
FRAME_WAIT_S = 0.02
MODEL_PATH = "/opt/hobot/model/x5/basic/yolov5s_v7_640x640_nv12.bin"
SCORE_TH = 0.4
NMS_TH = 0.45
NMS_TOP_K = 20
INFER_EVERY = 3
PASSTHROUGH_FRAMES = 30  # image d'abord, YOLO ensuite

camera = None
sock = None
ffmpeg_proc = None
reader_thread = None
stop_reader = threading.Event()
models = None
model_h = 640
model_w = 640
last_dets = []


class hbSysMem_t(ctypes.Structure):
    _fields_ = [
        ("phyAddr", ctypes.c_double),
        ("virAddr", ctypes.c_void_p),
        ("memSize", ctypes.c_int),
    ]


class hbDNNQuantiShift_yt(ctypes.Structure):
    _fields_ = [("shiftLen", ctypes.c_int), ("shiftData", ctypes.c_char_p)]


class hbDNNQuantiScale_t(ctypes.Structure):
    _fields_ = [
        ("scaleLen", ctypes.c_int),
        ("scaleData", ctypes.POINTER(ctypes.c_float)),
        ("zeroPointLen", ctypes.c_int),
        ("zeroPointData", ctypes.c_char_p),
    ]


class hbDNNTensorShape_t(ctypes.Structure):
    _fields_ = [
        ("dimensionSize", ctypes.c_int * 8),
        ("numDimensions", ctypes.c_int),
    ]


class hbDNNTensorProperties_t(ctypes.Structure):
    _fields_ = [
        ("validShape", hbDNNTensorShape_t),
        ("alignedShape", hbDNNTensorShape_t),
        ("tensorLayout", ctypes.c_int),
        ("tensorType", ctypes.c_int),
        ("shift", hbDNNQuantiShift_yt),
        ("scale", hbDNNQuantiScale_t),
        ("quantiType", ctypes.c_int),
        ("quantizeAxis", ctypes.c_int),
        ("alignedByteSize", ctypes.c_int),
        ("stride", ctypes.c_int * 8),
    ]


class hbDNNTensor_t(ctypes.Structure):
    _fields_ = [
        ("sysMem", hbSysMem_t * 4),
        ("properties", hbDNNTensorProperties_t),
    ]


class Yolov5PostProcessInfo_t(ctypes.Structure):
    _fields_ = [
        ("height", ctypes.c_int),
        ("width", ctypes.c_int),
        ("ori_height", ctypes.c_int),
        ("ori_width", ctypes.c_int),
        ("score_threshold", ctypes.c_float),
        ("nms_threshold", ctypes.c_float),
        ("nms_top_k", ctypes.c_int),
        ("is_pad_resize", ctypes.c_int),
    ]


libpostprocess = ctypes.CDLL("/usr/lib/libpostprocess.so")
get_Postprocess_result = libpostprocess.Yolov5PostProcess
get_Postprocess_result.argtypes = [ctypes.POINTER(Yolov5PostProcessInfo_t)]
get_Postprocess_result.restype = ctypes.c_char_p


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def get_TensorLayout(layout):
    return 2 if layout == "NCHW" else 0


def get_hw(pro):
    if pro.layout == "NCHW":
        return pro.shape[2], pro.shape[3]
    return pro.shape[1], pro.shape[2]


def bgr2nv12_opencv(image):
    height, width = image.shape[0], image.shape[1]
    area = height * width
    yuv420p = cv2.cvtColor(image, cv2.COLOR_BGR2YUV_I420).reshape((area * 3 // 2,))
    y = yuv420p[:area]
    uv_planar = yuv420p[area:].reshape((2, area // 4))
    uv_packed = uv_planar.transpose((1, 0)).reshape((area // 2,))
    nv12 = np.empty_like(yuv420p)
    nv12[:area] = y
    nv12[area:] = uv_packed
    return nv12


def nv12_to_bgr(nv12, width, height):
    arr = np.frombuffer(nv12, dtype=np.uint8, count=width * height * 3 // 2)
    yuv = arr.reshape((height * 3 // 2, width))
    return cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_NV12)


def load_model():
    global models, model_h, model_w
    from hobot_dnn import pyeasy_dnn as dnn
    log("loading model %s" % MODEL_PATH)
    models = dnn.load(MODEL_PATH)
    model_h, model_w = get_hw(models[0].inputs[0].properties)
    log("model ready %dx%d" % (model_w, model_h))


def run_yolo(bgr):
    resized = cv2.resize(bgr, (model_w, model_h), interpolation=cv2.INTER_AREA)
    nv12_data = bgr2nv12_opencv(resized)
    outputs = models[0].forward(nv12_data)

    info = Yolov5PostProcessInfo_t()
    info.height = model_h
    info.width = model_w
    info.ori_height = bgr.shape[0]
    info.ori_width = bgr.shape[1]
    info.score_threshold = SCORE_TH
    info.nms_threshold = NMS_TH
    info.nms_top_k = NMS_TOP_K
    info.is_pad_resize = 0

    output_tensors = (hbDNNTensor_t * len(models[0].outputs))()
    for i in range(len(models[0].outputs)):
        output_tensors[i].properties.tensorLayout = get_TensorLayout(
            outputs[i].properties.layout
        )
        if len(outputs[i].properties.scale_data) == 0:
            output_tensors[i].properties.quantiType = 0
            output_tensors[i].sysMem[0].virAddr = ctypes.cast(
                outputs[i].buffer.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                ctypes.c_void_p,
            )
        else:
            output_tensors[i].properties.quantiType = 2
            output_tensors[i].properties.scale.scaleData = (
                outputs[i].properties.scale_data.ctypes.data_as(
                    ctypes.POINTER(ctypes.c_float)
                )
            )
            output_tensors[i].sysMem[0].virAddr = ctypes.cast(
                outputs[i].buffer.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
                ctypes.c_void_p,
            )
        for j in range(len(outputs[i].properties.shape)):
            output_tensors[i].properties.validShape.dimensionSize[j] = (
                outputs[i].properties.shape[j]
            )
        libpostprocess.Yolov5doProcess(output_tensors[i], ctypes.pointer(info), i)

    result_str = get_Postprocess_result(ctypes.pointer(info)).decode("utf-8")
    return json.loads(result_str[16:])


def draw_dets(bgr, dets):
    for result in dets:
        bbox = result["bbox"]
        score = result["score"]
        name = result["name"]
        x1, y1, x2, y2 = int(bbox[0]), int(bbox[1]), int(bbox[2]), int(bbox[3])
        cv2.rectangle(bgr, (x1, y1), (x2, y2), (0, 255, 0), 2)
        cv2.putText(
            bgr,
            "%s %.2f" % (name, score),
            (x1, max(0, y1 - 8)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.5,
            (0, 255, 0),
            1,
        )


def cleanup():
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


def handle_signal(_s, _f):
    cleanup()
    sys.exit(0)


def start_ffmpeg_nv12(width, height, fps, bitrate):
    bps = max(int(bitrate), 300000)
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-fflags", "nobuffer", "-flags", "low_delay",
        "-f", "rawvideo", "-pix_fmt", "nv12",
        "-s:v", "%dx%d" % (width, height), "-r", str(fps), "-i", "pipe:0",
        "-an", "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.1",
        "-pix_fmt", "yuv420p", "-preset", "ultrafast", "-tune", "zerolatency",
        "-b:v", str(bps), "-maxrate", str(bps), "-bufsize", str(bps),
        "-g", str(fps), "-keyint_min", str(fps), "-bf", "0", "-sc_threshold", "0",
        "-threads", "2",
        "-x264-params", "repeat-headers=1:annexb=1:sliced-threads=0",
        "-flush_packets", "1", "-muxdelay", "0", "-muxpreload", "0",
        "-f", "h264", "pipe:1",
    ]
    return subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, bufsize=0,
    )


def start_socket_reader(client, source):
    def _run():
        try:
            while not stop_reader.is_set():
                chunk = source.read(65536)
                if not chunk:
                    break
                client.sendall(chunk)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    t = threading.Thread(target=_run, daemon=True)
    t.start()
    return t


def connect_video_sink(host, port):
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    while True:
        try:
            client.connect((host, port))
            return client
        except (ConnectionRefusedError, OSError):
            time.sleep(0.5)


def capture_frame():
    for _ in range(WARMUP_ATTEMPTS):
        frame = camera.get_img(2, CAP_WIDTH, CAP_HEIGHT)
        if frame is not None:
            return frame
        time.sleep(FRAME_WAIT_S)
    return None


def main():
    global camera, sock, ffmpeg_proc, reader_thread, last_dets

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    if len(sys.argv) < 4:
        log("usage: vigi-encode-yolo.py WIDTH HEIGHT FPS [BITRATE]")
        return 1

    req_w = int(sys.argv[1])
    req_h = int(sys.argv[2])
    fps = int(sys.argv[3])
    bitrate = int(sys.argv[4]) if len(sys.argv) > 4 else 600000
    fps = min(max(fps, 5), 15)
    bitrate = min(max(bitrate, 300000), 700000)
    expected = CAP_WIDTH * CAP_HEIGHT * 3 // 2

    log("yolo stream-first %dx%d@%d br=%d" % (CAP_WIDTH, CAP_HEIGHT, fps, bitrate))

    camera = libsrcampy.Camera()
    if camera.open_cam(0, -1, fps, CAP_WIDTH, CAP_HEIGHT) != 0:
        log("camera.open_cam failed")
        cleanup()
        return 1
    if capture_frame() is None:
        log("camera warmup failed")
        cleanup()
        return 1

    log("camera ready, connecting")
    sock = connect_video_sink("127.0.0.1", VIDEO_PORT)
    log("connected tcp://127.0.0.1:%d" % VIDEO_PORT)

    try:
        ffmpeg_proc = start_ffmpeg_nv12(CAP_WIDTH, CAP_HEIGHT, fps, bitrate)
        reader_thread = start_socket_reader(sock, ffmpeg_proc.stdout)
    except Exception as exc:
        log("ffmpeg start failed: %s" % exc)
        cleanup()
        return 1

    model_ok = False
    sent_frames = 0
    try:
        while True:
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
                # 1) Image tout de suite
                if sent_frames < PASSTHROUGH_FRAMES:
                    ffmpeg_proc.stdin.write(frame[:expected])
                else:
                    # 2) Charge le modèle une fois le flux démarré
                    if not model_ok:
                        load_model()
                        model_ok = True
                        log("switching to YOLO overlay")

                    bgr = nv12_to_bgr(frame[:expected], CAP_WIDTH, CAP_HEIGHT)
                    if (sent_frames - PASSTHROUGH_FRAMES) % INFER_EVERY == 0:
                        last_dets = run_yolo(bgr)
                    draw_dets(bgr, last_dets)
                    ffmpeg_proc.stdin.write(bgr2nv12_opencv(bgr).tobytes())
                ffmpeg_proc.stdin.flush()
            except BrokenPipeError:
                log("ffmpeg stdin closed")
                return 1
            except Exception as exc:
                log("frame error: %s" % exc)
                try:
                    ffmpeg_proc.stdin.write(frame[:expected])
                    ffmpeg_proc.stdin.flush()
                except BrokenPipeError:
                    return 1

            sent_frames += 1
            if sent_frames == 1 or sent_frames % 30 == 0:
                log("sent %d frames (dets=%d model=%s)" % (
                    sent_frames, len(last_dets), model_ok))
            time.sleep(0.001)

    except (BrokenPipeError, ConnectionResetError):
        log("video sink disconnected")
        return 0
    finally:
        cleanup()
    return 0


if __name__ == "__main__":
    sys.exit(main())
