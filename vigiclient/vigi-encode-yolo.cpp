/*
 * Vigibot RDK X5 — YOLO overlay encoder (full C++)
 *   SP cam NV12 640x480 -> BPU YOLOv5s_v7 -> draw -> libx264 CB -> TCP :8043
 *
 * Usage: vigi-encode-yolo WIDTH HEIGHT FPS [BITRATE_BPS]
 */
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>

extern "C" {
#include <x264.h>
#include "sp_vio.h"
#include "sp_bpu.h"
}

#include "yolov5_post_process.hpp"

static std::atomic<bool> g_run{true};
static void on_sig(int) { g_run = false; }

static const char *kDefaultModel =
    "/opt/hobot/model/x5/basic/yolov5s_v7_640x640_nv12.bin";
static const int kModel = 640;
static const int kPassthrough = 30;
static const int kInferEvery = 3;
static const float kNmsTh = 0.45f;
static const int kNmsTopK = 20;

static int connect_8043(int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  while (g_run) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int snd = 16 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      return fd;
    }
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return -1;
}

static bool send_all(int fd, const void *p, size_t n) {
  auto *b = static_cast<const uint8_t *>(p);
  while (n) {
    ssize_t w = ::send(fd, b, n, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (w == 0)
      return false;
    b += static_cast<size_t>(w);
    n -= static_cast<size_t>(w);
  }
  return true;
}

static bool send_nal_annexb4(int fd, const x264_nal_t &nal) {
  static const uint8_t sc4[4] = {0, 0, 0, 1};
  const uint8_t *p = nal.p_payload;
  int n = nal.i_payload;
  if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
    p += 4;
    n -= 4;
  } else if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
    p += 3;
    n -= 3;
  }
  if (n <= 0)
    return true;
  if (!send_all(fd, sc4, 4))
    return false;
  return send_all(fd, p, static_cast<size_t>(n));
}

struct X264Enc {
  x264_t *h = nullptr;
  x264_param_t param{};
  x264_picture_t pic{};
  x264_picture_t pic_out{};
  int W = 0;
  int H = 0;

  bool open(int width, int height, int fps, int bitrate_bps) {
    W = width;
    H = height;
    if (x264_param_default_preset(&param, "veryfast", "zerolatency") < 0)
      return false;
    param.i_csp = X264_CSP_NV12;
    param.i_width = W;
    param.i_height = H;
    param.i_fps_num = fps;
    param.i_fps_den = 1;
    param.i_keyint_max = fps;
    param.i_keyint_min = fps;
    param.b_repeat_headers = 1;
    param.b_annexb = 1;
    param.i_bframe = 0;
    param.b_cabac = 0;
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = 28;
    param.rc.i_vbv_max_bitrate = bitrate_bps / 1000;
    param.rc.i_vbv_buffer_size = std::max(bitrate_bps / 8000, 20);
    param.rc.i_qp_min = 18;
    param.rc.i_qp_max = 40;
    param.i_threads = 1;
    param.b_sliced_threads = 0;
    param.i_sync_lookahead = 0;
    param.rc.i_lookahead = 0;
    param.b_vfr_input = 0;
    param.i_scenecut_threshold = 0;
    param.analyse.i_me_method = X264_ME_HEX;
    param.analyse.i_subpel_refine = 2;
    if (x264_param_apply_profile(&param, "baseline") < 0)
      return false;
    param.i_level_idc = 31;
    h = x264_encoder_open(&param);
    if (!h)
      return false;
    if (x264_picture_alloc(&pic, param.i_csp, param.i_width, param.i_height) < 0) {
      x264_encoder_close(h);
      h = nullptr;
      return false;
    }
    return true;
  }

  void close() {
    if (h) {
      x264_picture_clean(&pic);
      x264_encoder_close(h);
      h = nullptr;
    }
  }

  int encode_nv12(const uint8_t *nv12, int64_t pts, int sock, bool force_key) {
    const int y_size = W * H;
    std::memcpy(pic.img.plane[0], nv12, static_cast<size_t>(y_size));
    std::memcpy(pic.img.plane[1], nv12 + y_size, static_cast<size_t>(y_size / 2));
    pic.i_pts = pts;
    pic.i_type = force_key ? X264_TYPE_IDR : X264_TYPE_AUTO;
    x264_nal_t *nals = nullptr;
    int nal_count = 0;
    int frame_size =
        x264_encoder_encode(h, &nals, &nal_count, &pic, &pic_out);
    if (frame_size < 0)
      return -1;
    if (frame_size == 0)
      return 0;
    for (int i = 0; i < nal_count; ++i) {
      if (!send_nal_annexb4(sock, nals[i])) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return -2;
        return -1;
      }
    }
    return frame_size;
  }
};

static void bgr_to_nv12(const cv::Mat &bgr, uint8_t *nv12) {
  cv::Mat yuv;
  cv::cvtColor(bgr, yuv, cv::COLOR_BGR2YUV_I420);
  const int w = bgr.cols;
  const int h = bgr.rows;
  const int y_size = w * h;
  std::memcpy(nv12, yuv.data, static_cast<size_t>(y_size));
  const uint8_t *u = yuv.data + y_size;
  const uint8_t *v = u + y_size / 4;
  uint8_t *uv = nv12 + y_size;
  for (int i = 0; i < y_size / 4; ++i) {
    uv[2 * i] = u[i];
    uv[2 * i + 1] = v[i];
  }
}

static void draw_dets(cv::Mat &bgr, const std::vector<YoloV5Result> &dets) {
  for (const auto &d : dets) {
    cv::rectangle(bgr,
                  cv::Point(static_cast<int>(d.xmin), static_cast<int>(d.ymin)),
                  cv::Point(static_cast<int>(d.xmax), static_cast<int>(d.ymax)),
                  cv::Scalar(0, 255, 0), 2);
    char label[64];
    std::snprintf(label, sizeof(label), "%s %.2f", d.class_name.c_str(), d.score);
    int y = std::max(0, static_cast<int>(d.ymin) - 8);
    cv::putText(bgr, label, cv::Point(static_cast<int>(d.xmin), y),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
  }
}

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s WIDTH HEIGHT FPS [BITRATE_BPS]\n", argv[0]);
    return 1;
  }

  int W = std::atoi(argv[1]);
  int H = std::atoi(argv[2]);
  int fps = std::atoi(argv[3]);
  int br = argc > 4 ? std::atoi(argv[4]) : 100000;
  if (W < 640)
    W = 640;
  if (H < 480)
    H = 480;
  W &= ~1;
  H &= ~1;
  if (fps < 5)
    fps = 8;
  if (fps > 8)
    fps = 8;
  if (br < 70000)
    br = 70000;
  if (br > 100000)
    br = 100000;

  const char *model_path =
      std::getenv("VIGI_YOLO_MODEL") ? std::getenv("VIGI_YOLO_MODEL")
                                    : kDefaultModel;
  const int port =
      std::getenv("VIGI_PORT") ? std::atoi(std::getenv("VIGI_PORT")) : 8043;

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  std::fprintf(stderr,
               "vigi-encode-yolo: SP+BPU+x264 %dx%d@%d br=%d model=%s -> :%d\n",
               W, H, fps, br, model_path, port);

  void *cam = sp_init_vio_module();
  if (!cam) {
    std::fprintf(stderr, "sp_init_vio_module failed\n");
    return 1;
  }

  sp_sensors_parameters sens{};
  sens.fps = 15;
  sens.raw_width = -1;
  sens.raw_height = -1;
  int32_t widths[1] = {W};
  int32_t heights[1] = {H};
  bool cam_ok = false;
  for (int attempt = 1; attempt <= 12; ++attempt) {
    if (sp_open_camera_v2(cam, 0, -1, 1, &sens, widths, heights) == 0) {
      cam_ok = true;
      break;
    }
    std::fprintf(stderr, "sp_open_camera_v2 failed (try %d/12)\n", attempt);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (!cam_ok) {
    std::fprintf(stderr, "camera open failed permanently\n");
    sp_release_vio_module(cam);
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  std::fprintf(stderr, "camera ready\n");

  X264Enc enc;
  if (!enc.open(W, H, fps, br)) {
    std::fprintf(stderr, "libx264 open failed\n");
    sp_vio_close(cam);
    sp_release_vio_module(cam);
    return 1;
  }

  bpu_module *bpu = nullptr;
  hbDNNTensor out_tensors[2][3];
  int tensor_idx = 0;
  bool bpu_ready = false;

  const size_t nv12_sz = static_cast<size_t>(W) * static_cast<size_t>(H) * 3 / 2;
  const size_t model_nv12_sz =
      static_cast<size_t>(kModel) * static_cast<size_t>(kModel) * 3 / 2;
  std::vector<char> frame(nv12_sz);
  std::vector<uint8_t> model_nv12(model_nv12_sz);
  std::vector<uint8_t> out_nv12(nv12_sz);
  std::vector<YoloV5Result> last_dets;

  bpu_image_info_t img_info{};
  img_info.m_model_w = kModel;
  img_info.m_model_h = kModel;
  img_info.m_ori_width = W;
  img_info.m_ori_height = H;

  int64_t pts = 0;
  uint64_t n = 0;
  uint64_t sent = 0;
  int sock = -1;
  bool need_key = true;
  int cam_tick = 0;
  auto t_log = std::chrono::steady_clock::now();
  uint64_t n_log = 0;

  while (g_run) {
    if (sock < 0) {
      std::fprintf(stderr, "connecting tcp://127.0.0.1:%d ...\n", port);
      sock = connect_8043(port);
      if (sock < 0)
        break;
      std::fprintf(stderr, "connected tcp://127.0.0.1:%d\n", port);
      need_key = true;
    }

    auto t0 = std::chrono::steady_clock::now();
    if (sp_vio_get_frame(cam, frame.data(), W, H, 2000) != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    /* Capture ~15 fps, encode ~8 fps. */
    if ((++cam_tick & 1) && !need_key && sent >= kPassthrough)
      continue;

    const uint8_t *encode_ptr =
        reinterpret_cast<const uint8_t *>(frame.data());

    if (sent < static_cast<uint64_t>(kPassthrough)) {
      /* Stream-first: raw frames before loading / running YOLO. */
      if (sent + 1 == static_cast<uint64_t>(kPassthrough) && !bpu_ready) {
        std::fprintf(stderr, "loading BPU model %s\n", model_path);
        bpu = sp_init_bpu_module(model_path);
        if (!bpu) {
          std::fprintf(stderr, "sp_init_bpu_module failed — raw only\n");
        } else {
          int ok = 1;
          for (int i = 0; i < 2; ++i) {
            if (sp_init_bpu_tensors(bpu, out_tensors[i]) != 0) {
              std::fprintf(stderr, "sp_init_bpu_tensors failed\n");
              ok = 0;
              break;
            }
          }
          if (ok) {
            bpu_ready = true;
            std::fprintf(stderr, "YOLO overlay armed\n");
          }
        }
      }
    } else if (bpu_ready) {
      cv::Mat nv12_in(H * 3 / 2, W, CV_8UC1,
                      reinterpret_cast<uint8_t *>(frame.data()));
      cv::Mat bgr;
      cv::cvtColor(nv12_in, bgr, cv::COLOR_YUV2BGR_NV12);

      if ((sent - kPassthrough) % kInferEvery == 0) {
        cv::Mat resized;
        cv::resize(bgr, resized, cv::Size(kModel, kModel), 0, 0,
                   cv::INTER_AREA);
        bgr_to_nv12(resized, model_nv12.data());

        bpu->output_tensor = &out_tensors[tensor_idx][0];
        if (sp_bpu_start_predict(bpu, reinterpret_cast<char *>(model_nv12.data())) ==
            0) {
          std::vector<YoloV5Result> parsed;
          parsed.reserve(64);
          for (int j = 0; j < 3; ++j)
            ParseTensor(&out_tensors[tensor_idx][j], j, parsed, img_info);
          last_dets.clear();
          yolo5_nms(parsed, kNmsTh, kNmsTopK, last_dets, false);
          tensor_idx ^= 1;
        }
      }

      draw_dets(bgr, last_dets);
      bgr_to_nv12(bgr, out_nv12.data());
      encode_ptr = out_nv12.data();
    }

    int got = enc.encode_nv12(encode_ptr, pts++, sock, need_key);
    if (got == -2) {
      need_key = true;
      continue;
    }
    if (got < 0) {
      std::fprintf(stderr, "send failed — reconnecting\n");
      close(sock);
      sock = -1;
      need_key = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    need_key = false;
    ++n;
    ++n_log;
    ++sent;

    auto now = std::chrono::steady_clock::now();
    if (now - t_log >= std::chrono::seconds(2)) {
      double sec = std::chrono::duration<double>(now - t_log).count();
      double ms = std::chrono::duration<double, std::milli>(now - t0).count();
      std::fprintf(stderr,
                   "fps=%.1f encode_ms=%.1f dets=%zu frames=%llu yolo=%d\n",
                   n_log / sec, ms, last_dets.size(),
                   static_cast<unsigned long long>(n), bpu_ready ? 1 : 0);
      t_log = now;
      n_log = 0;
    }
  }

  enc.close();
  if (bpu_ready && bpu) {
    for (int i = 0; i < 2; ++i)
      sp_deinit_bpu_tensor(out_tensors[i], 3);
    sp_release_bpu_module(bpu);
  }
  sp_vio_close(cam);
  sp_release_vio_module(cam);
  if (sock >= 0)
    close(sock);
  return 0;
}
