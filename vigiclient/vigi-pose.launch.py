#!/usr/bin/env python3
"""Minimal TROS pipeline for Vigibot body-keypoint streaming."""

import os

from ament_index_python import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def include_launch(package, relative_path, arguments=None):
    return IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(get_package_share_directory(package), relative_path)
        ),
        launch_arguments=(arguments or {}).items(),
    )


def generate_launch_description():
    device = DeclareLaunchArgument(
        "device",
        default_value="IMX219",
        description="MIPI camera sensor",
    )

    shared_memory = include_launch("hobot_shm", "launch/hobot_shm.launch.py")
    camera = include_launch(
        "mipi_cam",
        "launch/mipi_cam.launch.py",
        {
            "mipi_image_width": "960",
            "mipi_image_height": "544",
            "mipi_io_method": "shared_mem",
            "mipi_frame_ts_type": "realtime",
            "mipi_video_device": LaunchConfiguration("device"),
        },
    )
    jpeg_encoder = include_launch(
        "hobot_codec",
        "launch/hobot_codec_encode.launch.py",
        {
            "codec_in_mode": "shared_mem",
            "codec_out_mode": "ros",
            "codec_sub_topic": "/hbmem_img",
            "codec_pub_topic": "/image",
        },
    )
    detector = Node(
        package="mono2d_body_detection",
        executable="mono2d_body_detection",
        output="screen",
        parameters=[
            {
                "model_file_name": (
                    "/opt/tros/humble/lib/mono2d_body_detection/config/"
                    "multitask_body_head_face_hand_kps_960x544.hbm"
                ),
                "ai_msg_pub_topic_name": "/hobot_mono2d_body_detection",
                "image_gap": 2,
            }
        ],
        arguments=["--ros-args", "--log-level", "warn"],
    )

    return LaunchDescription(
        [device, shared_memory, camera, jpeg_encoder, detector]
    )
