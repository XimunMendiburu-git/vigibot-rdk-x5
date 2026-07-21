#!/usr/bin/env python3
"""Body-keypoint overlay source for Vigibot on RDK X5.

The official TROS mono2d body detector owns the IMX219, publishes JPEG frames
and ai_msgs body keypoints, and this process draws the skeleton before sending
an H.264 Annex-B stream to the Vigibot client on TCP 8043.
"""

import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

import cv2
import numpy as np
import rclpy
from ai_msgs.msg import PerceptionTargets
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from rclpy.qos import HistoryPolicy, QoSProfile, ReliabilityPolicy
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage


VIDEO_PORT = 8043
CAP_WIDTH = 960
CAP_HEIGHT = 544
OUT_WIDTH = 640
OUT_HEIGHT = 480
MODEL_CONFIG = "/opt/tros/humble/lib/mono2d_body_detection/config"
LAUNCH_FILE = "/usr/local/vigiclient/vigi-pose.launch.py"
POSE_TOPIC = "/hobot_mono2d_body_detection"
IMAGE_TOPIC = "/image"
POSE_TIMEOUT_S = 0.75
KEYPOINT_SCORE = 0.25

KEYPOINT_NAMES = (
    "nose",
    "left_eye",
    "right_eye",
    "left_ear",
    "right_ear",
    "left_shoulder",
    "right_shoulder",
    "left_elbow",
    "right_elbow",
    "left_wrist",
    "right_wrist",
    "left_hip",
    "right_hip",
    "left_knee",
    "right_knee",
    "left_ankle",
    "right_ankle",
)
SKELETON = (
    (5, 6),
    (5, 7),
    (7, 9),
    (6, 8),
    (8, 10),
    (5, 11),
    (6, 12),
    (11, 12),
    (11, 13),
    (13, 15),
    (12, 14),
    (14, 16),
    (0, 1),
    (0, 2),
    (1, 3),
    (2, 4),
)

shutdown_requested = threading.Event()
reader_stop = threading.Event()
launch_proc = None
ffmpeg_proc = None
sock = None
executor = None
ros_node = None
spin_thread = None
reader_thread = None


def log(message):
    print(message, file=sys.stderr, flush=True)


class PoseSubscriber(Node):
    def __init__(self):
        super().__init__("vigibot_pose_subscriber")
        self.lock = threading.Lock()
        self.frame_ready = threading.Condition(self.lock)
        self.jpeg = None
        self.frame_id = 0
        self.poses = []
        self.pose_time = 0.0

        qos = QoSProfile(
            history=HistoryPolicy.KEEP_LAST,
            depth=1,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self.create_subscription(
            CompressedImage,
            IMAGE_TOPIC,
            self.on_image,
            qos_profile_sensor_data,
        )
        self.create_subscription(
            PerceptionTargets, POSE_TOPIC, self.on_pose, qos
        )

    def on_image(self, message):
        with self.frame_ready:
            self.jpeg = bytes(message.data)
            self.frame_id += 1
            self.frame_ready.notify()

    def on_pose(self, message):
        poses = []
        for target in message.targets:
            body_roi = None
            for roi in target.rois:
                if roi.type == "body":
                    body_roi = (
                        int(roi.rect.x_offset),
                        int(roi.rect.y_offset),
                        int(roi.rect.width),
                        int(roi.rect.height),
                    )
                    break

            for point_set in target.points:
                if point_set.type != "body_kps":
                    continue
                points = [
                    (float(point.x), float(point.y))
                    for point in point_set.point
                ]
                confidence = [float(value) for value in point_set.confidence]
                poses.append(
                    {
                        "track_id": int(target.track_id),
                        "roi": body_roi,
                        "points": points,
                        "confidence": confidence,
                    }
                )

        with self.lock:
            self.poses = poses
            self.pose_time = time.monotonic()

    def wait_for_frame(self, previous_id, timeout=1.0):
        with self.frame_ready:
            self.frame_ready.wait_for(
                lambda: self.frame_id != previous_id
                or shutdown_requested.is_set(),
                timeout=timeout,
            )
            if self.jpeg is None or self.frame_id == previous_id:
                return previous_id, None, []
            poses = (
                list(self.poses)
                if time.monotonic() - self.pose_time <= POSE_TIMEOUT_S
                else []
            )
            return self.frame_id, self.jpeg, poses


def prepare_ros_workdir():
    workdir = "/tmp/vigibot-pose"
    target = os.path.join(workdir, "config")
    os.makedirs(target, exist_ok=True)
    for name in os.listdir(MODEL_CONFIG):
        source = os.path.join(MODEL_CONFIG, name)
        destination = os.path.join(target, name)
        if os.path.isfile(source):
            shutil.copy2(source, destination)
    return workdir


def start_ros_pipeline():
    workdir = prepare_ros_workdir()
    env = os.environ.copy()
    env["CAM_TYPE"] = "mipi"
    env["ROS_LOG_DIR"] = "/tmp/vigibot-pose/ros-logs"
    os.makedirs(env["ROS_LOG_DIR"], exist_ok=True)
    command = [
        "ros2",
        "launch",
        LAUNCH_FILE,
        "device:=IMX219",
    ]
    log("starting TROS body-keypoint pipeline")
    return subprocess.Popen(
        command,
        cwd=workdir,
        env=env,
        start_new_session=True,
    )


def start_ffmpeg(width, height, fps, bitrate):
    command = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-fflags",
        "nobuffer",
        "-flags",
        "low_delay",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "bgr24",
        "-s:v",
        f"{width}x{height}",
        "-r",
        str(fps),
        "-i",
        "pipe:0",
        "-an",
        "-c:v",
        "libx264",
        "-profile:v",
        "baseline",
        "-level",
        "3.1",
        "-pix_fmt",
        "yuv420p",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-b:v",
        str(bitrate),
        "-maxrate",
        str(bitrate),
        "-bufsize",
        str(bitrate),
        "-g",
        str(fps),
        "-keyint_min",
        str(fps),
        "-bf",
        "0",
        "-sc_threshold",
        "0",
        "-threads",
        "2",
        "-x264-params",
        "repeat-headers=1:annexb=1:sliced-threads=0",
        "-flush_packets",
        "1",
        "-f",
        "h264",
        "pipe:1",
    ]
    return subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )


def connect_video_sink():
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    while not shutdown_requested.is_set():
        try:
            client.connect(("127.0.0.1", VIDEO_PORT))
            return client
        except (ConnectionRefusedError, OSError):
            time.sleep(0.5)
    raise RuntimeError("shutdown before video sink connection")


def start_socket_reader(client, source):
    def run():
        try:
            while not reader_stop.is_set():
                chunk = source.read(65536)
                if not chunk:
                    break
                client.sendall(chunk)
        except (BrokenPipeError, ConnectionResetError, OSError):
            shutdown_requested.set()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread


def scale_point(point):
    return (
        int(point[0] * OUT_WIDTH / CAP_WIDTH),
        int(point[1] * OUT_HEIGHT / CAP_HEIGHT),
    )


def draw_poses(frame, poses):
    for pose in poses:
        points = pose["points"]
        confidence = pose["confidence"]
        if len(points) < len(KEYPOINT_NAMES):
            continue

        for start, end in SKELETON:
            if (
                start >= len(confidence)
                or end >= len(confidence)
                or confidence[start] < KEYPOINT_SCORE
                or confidence[end] < KEYPOINT_SCORE
            ):
                continue
            cv2.line(
                frame,
                scale_point(points[start]),
                scale_point(points[end]),
                (0, 220, 255),
                2,
                cv2.LINE_AA,
            )

        for index, point in enumerate(points[: len(KEYPOINT_NAMES)]):
            if index >= len(confidence) or confidence[index] < KEYPOINT_SCORE:
                continue
            cv2.circle(
                frame,
                scale_point(point),
                4,
                (255, 80, 80),
                -1,
                cv2.LINE_AA,
            )

        roi = pose["roi"]
        if roi is not None:
            x, y, width, height = roi
            p1 = scale_point((x, y))
            p2 = scale_point((x + width, y + height))
            cv2.rectangle(frame, p1, p2, (80, 255, 80), 2)
            cv2.putText(
                frame,
                f"person #{pose['track_id']}",
                (p1[0], max(16, p1[1] - 7)),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.5,
                (80, 255, 80),
                1,
                cv2.LINE_AA,
            )

    cv2.putText(
        frame,
        f"POSE people={len(poses)}",
        (10, 24),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.65,
        (0, 220, 255),
        2,
        cv2.LINE_AA,
    )


def request_shutdown(_signal_number=None, _frame=None):
    shutdown_requested.set()


def cleanup():
    global launch_proc, ffmpeg_proc, sock, executor, ros_node

    reader_stop.set()
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
            ffmpeg_proc.kill()
        ffmpeg_proc = None

    if sock is not None:
        try:
            sock.close()
        except OSError:
            pass
        sock = None

    if executor is not None:
        executor.shutdown(timeout_sec=2.0)
        executor = None
    if ros_node is not None:
        ros_node.destroy_node()
        ros_node = None
    if rclpy.ok():
        rclpy.shutdown()

    if launch_proc is not None:
        try:
            os.killpg(launch_proc.pid, signal.SIGTERM)
            launch_proc.wait(timeout=5)
        except Exception:
            try:
                os.killpg(launch_proc.pid, signal.SIGKILL)
            except OSError:
                pass
        launch_proc = None


def main():
    global launch_proc, ffmpeg_proc, sock, executor
    global ros_node, spin_thread, reader_thread

    signal.signal(signal.SIGTERM, request_shutdown)
    signal.signal(signal.SIGINT, request_shutdown)

    if len(sys.argv) < 4:
        log("usage: vigi-encode-pose.py WIDTH HEIGHT FPS [BITRATE]")
        return 1

    fps = min(max(int(sys.argv[3]), 5), 15)
    bitrate = int(sys.argv[4]) if len(sys.argv) > 4 else 700000
    bitrate = min(max(bitrate, 300000), 900000)

    try:
        launch_proc = start_ros_pipeline()
        rclpy.init(args=None)
        ros_node = PoseSubscriber()
        executor = SingleThreadedExecutor()
        executor.add_node(ros_node)
        spin_thread = threading.Thread(target=executor.spin, daemon=True)
        spin_thread.start()

        frame_id = 0
        deadline = time.monotonic() + 20
        jpeg = None
        poses = []
        while time.monotonic() < deadline and not shutdown_requested.is_set():
            if launch_proc.poll() is not None:
                raise RuntimeError(
                    f"TROS pipeline exited with code {launch_proc.returncode}"
                )
            frame_id, jpeg, poses = ros_node.wait_for_frame(frame_id, 1.0)
            if jpeg is not None:
                break
        if jpeg is None:
            raise RuntimeError("timed out waiting for TROS camera frames")

        sock = connect_video_sink()
        ffmpeg_proc = start_ffmpeg(OUT_WIDTH, OUT_HEIGHT, fps, bitrate)
        reader_thread = start_socket_reader(sock, ffmpeg_proc.stdout)
        log(
            f"pose source ready {OUT_WIDTH}x{OUT_HEIGHT}@{fps} "
            f"bitrate={bitrate}"
        )

        frame_interval = 1.0 / fps
        next_frame = time.monotonic()
        sent = 0
        while not shutdown_requested.is_set():
            frame_id, jpeg, poses = ros_node.wait_for_frame(frame_id, 1.0)
            if jpeg is None:
                if launch_proc.poll() is not None:
                    raise RuntimeError("TROS pipeline stopped")
                continue

            image = cv2.imdecode(
                np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR
            )
            if image is None:
                continue
            if image.shape[1] != CAP_WIDTH or image.shape[0] != CAP_HEIGHT:
                image = cv2.resize(image, (CAP_WIDTH, CAP_HEIGHT))
            image = cv2.resize(
                image, (OUT_WIDTH, OUT_HEIGHT), interpolation=cv2.INTER_AREA
            )
            draw_poses(image, poses)

            wait = next_frame - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            next_frame = max(next_frame + frame_interval, time.monotonic())

            ffmpeg_proc.stdin.write(image.tobytes())
            ffmpeg_proc.stdin.flush()
            sent += 1
            if sent == 1 or sent % 30 == 0:
                log(f"sent {sent} pose frames (people={len(poses)})")

        return 0
    except (BrokenPipeError, ConnectionResetError):
        log("video sink disconnected")
        return 0
    except Exception as error:
        log(f"pose source failed: {error}")
        return 1
    finally:
        cleanup()


if __name__ == "__main__":
    sys.exit(main())
