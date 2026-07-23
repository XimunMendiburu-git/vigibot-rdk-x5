#!/usr/bin/env python3
"""
Vigibot RDK X5 — YOLO overlay (stream-first):
  Camera NV12 → (warmup) → YOLOv5s_v7 draw → vigi-encode-x264 --stdin → TCP 8043
"""
import ctypes
import json
import os
import signal
import subprocess
import sys
import time

import cv2
import numpy as np

try:
    from hobot_vio import libsrcampy
except ImportError:
    from hobot_vio_rdkx5 import libsrcampy

CAP_WIDTH = 640
CAP_HEIGHT = 480
CAM_FPS = 15
OUT_FPS = 8
MIN_BITRATE = 70000
MAX_BITRATE = 100000
WARMUP_ATTEMPTS = 200
FRAME_WAIT_S = 0.02
MODEL_PATH = "/opt/hobot/model/x5/basic/yolov5s_v7_640x640_nv12.bin"
X264_BIN = os.environ.get("VIGI_X264", "/usr/local/vigiclient/vigi-encode-x264")
SCORE_TH = 0.4
NMS_TH = 0.45
NMS_TOP_K = 20
INFER_EVERY = 3
PASSTHROUGH_FRAMES = 30

camera = None
x264_proc = None
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


def clamp_stream_params(fps, bitrate):
    fps = min(max(int(fps), 5), OUT_FPS)
    bitrate = min(max(int(bitrate), MIN_BITRATE), MAX_BITRATE)
    return fps, bitrate


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
    global camera, x264_proc
    if x264_proc is not None:
        try:
            if x264_proc.stdin:
                x264_proc.stdin.close()
        except OSError:
            pass
        try:
            x264_proc.terminate()
            x264_proc.wait(timeout=2)
        except Exception:
            try:
                x264_proc.kill()
            except Exception:
                pass
        x264_proc = None
    if camera is not None:
        try:
            camera.close_cam()
        except Exception:
            pass
        camera = None


def handle_signal(_s, _f):
    cleanup()
    sys.exit(0)


def start_x264_stdin(width, height, fps, bitrate):
    if not os.access(X264_BIN, os.X_OK):
        raise FileNotFoundError("missing encoder: %s" % X264_BIN)
    cmd = [
        X264_BIN, "--stdin",
        str(width), str(height), str(fps), str(bitrate),
    ]
    log("starting %s" % " ".join(cmd))
    return subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )


def open_camera_with_retry():
    global camera
    camera = libsrcampy.Camera()
    for attempt in range(1, 13):
        if camera.open_cam(0, -1, CAM_FPS, CAP_WIDTH, CAP_HEIGHT) == 0:
            log("camera ready %dx%d@%d" % (CAP_WIDTH, CAP_HEIGHT, CAM_FPS))
            return True
        log("camera.open_cam failed (try %d/12)" % attempt)
        time.sleep(0.5)
    return False


def capture_frame():
    for _ in range(WARMUP_ATTEMPTS):
        frame = camera.get_img(2, CAP_WIDTH, CAP_HEIGHT)
        if frame is not None:
            return frame
        time.sleep(FRAME_WAIT_S)
    return None


def write_nv12(payload):
    x264_proc.stdin.write(payload)
    x264_proc.stdin.flush()


def main():
    global camera, x264_proc, last_dets

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    if len(sys.argv) < 4:
        log("usage: vigi-encode-yolo.py WIDTH HEIGHT FPS [BITRATE]")
        return 1

    fps, bitrate = clamp_stream_params(
        int(sys.argv[3]),
        int(sys.argv[4]) if len(sys.argv) > 4 else MAX_BITRATE,
    )
    expected = CAP_WIDTH * CAP_HEIGHT * 3 // 2
    frame_interval = 1.0 / float(fps)

    log("yolo stream-first %dx%d@%d br=%d (x264 stdin)" % (
        CAP_WIDTH, CAP_HEIGHT, fps, bitrate))

    if not open_camera_with_retry():
        log("camera.open_cam failed permanently")
        cleanup()
        return 1
    if capture_frame() is None:
        log("camera warmup failed")
        cleanup()
        return 1

    try:
        x264_proc = start_x264_stdin(CAP_WIDTH, CAP_HEIGHT, fps, bitrate)
    except Exception as exc:
        log("x264 start failed: %s" % exc)
        cleanup()
        return 1

    model_ok = False
    sent_frames = 0
    next_frame_at = time.monotonic()
    try:
        while True:
            frame = camera.get_img(2, CAP_WIDTH, CAP_HEIGHT)
            if frame is None or len(frame) < expected:
                time.sleep(FRAME_WAIT_S)
                continue

            now = time.monotonic()
            if sent_frames >= PASSTHROUGH_FRAMES and now < next_frame_at:
                continue
            next_frame_at = now + frame_interval

            try:
                if sent_frames < PASSTHROUGH_FRAMES:
                    write_nv12(frame[:expected])
                else:
                    if not model_ok:
                        load_model()
                        model_ok = True
                        log("switching to YOLO overlay")

                    bgr = nv12_to_bgr(frame[:expected], CAP_WIDTH, CAP_HEIGHT)
                    if (sent_frames - PASSTHROUGH_FRAMES) % INFER_EVERY == 0:
                        last_dets = run_yolo(bgr)
                    draw_dets(bgr, last_dets)
                    write_nv12(bgr2nv12_opencv(bgr).tobytes())
            except BrokenPipeError:
                log("x264 stdin closed")
                return 1
            except Exception as exc:
                log("frame error: %s" % exc)
                try:
                    write_nv12(frame[:expected])
                except BrokenPipeError:
                    return 1

            sent_frames += 1
            if sent_frames == 1 or sent_frames % 30 == 0:
                log("sent %d frames (dets=%d model=%s)" % (
                    sent_frames, len(last_dets), model_ok))

    except BrokenPipeError:
        log("encoder disconnected")
        return 0
    finally:
        cleanup()
    return 0


if __name__ == "__main__":
    sys.exit(main())
