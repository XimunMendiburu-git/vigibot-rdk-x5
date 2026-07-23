/*
 * Vigibot RDK X5 — body-pose overlay encoder (full C++, no TROS/ROS)
 *   SP cam NV12 640x480 -> BPU multitask body+kps 960x544 -> draw -> libx264 -> :8043
 *
 * Usage: vigi-encode-pose WIDTH HEIGHT FPS [BITRATE_BPS]
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

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

extern "C" {
#include <x264.h>
#include "sp_vio.h"
#include "sp_bpu.h"
}
#include <dnn/hb_sys.h>

#include "pose_post_process.hpp"

static std::atomic<bool> g_run{true};
static void on_sig(int) { g_run = false; }

static const char *kDefaultModel =
    "/opt/tros/humble/lib/mono2d_body_detection/config/"
    "multitask_body_head_face_hand_kps_960x544.hbm";
static const int kModelW = 960;
static const int kModelH = 544;
static const int kBodyBoxIdx = 1;
static const int kKpsIdx = 8;
static const int kOutCount = 9;
static const int kPassthrough = 20;
static const int kInferEvery = 2;
static const float kBoxScoreTh = 0.3f;
static const float kKpsScoreTh = 0.2f;

/* This .hbm expects NV12_SEPARATE (Y + UV planes). sp_bpu only allocs one
 * contiguous buffer — fix input tensors before the first predict. */
static int prepare_nv12_separate_input(bpu_module *bpu) {
  if (!bpu)
    return -1;
  auto &in = bpu->input_tensor;
  if (in.properties.tensorType != HB_DNN_IMG_TYPE_NV12_SEPARATE)
    return 0;
  const int h = in.properties.validShape.dimensionSize[2];
  const int w = in.properties.validShape.dimensionSize[3];
  const int y_size = h * w;
  const int uv_size = y_size / 2;
  if (in.sysMem[0].virAddr)
    hbSysFreeMem(&in.sysMem[0]);
  if (in.sysMem[1].virAddr)
    hbSysFreeMem(&in.sysMem[1]);
  std::memset(&in.sysMem[0], 0, sizeof(in.sysMem[0]));
  std::memset(&in.sysMem[1], 0, sizeof(in.sysMem[1]));
  if (hbSysAllocCachedMem(&in.sysMem[0], y_size) != 0)
    return -1;
  if (hbSysAllocCachedMem(&in.sysMem[1], uv_size) != 0)
    return -1;
  std::fprintf(stderr,
               "BPU input fixed to NV12_SEPARATE %dx%d (Y=%d UV=%d)\n", w, h,
               y_size, uv_size);
  return 0;
}

static int pose_bpu_predict(bpu_module *bpu, char *nv12) {
  if (!bpu || !nv12 || !bpu->output_tensor)
    return -1;
  auto &in = bpu->input_tensor;
  const int h = in.properties.validShape.dimensionSize[2];
  const int w = in.properties.validShape.dimensionSize[3];
  const int y_size = h * w;
  const int uv_size = y_size / 2;

  if (in.properties.tensorType == HB_DNN_IMG_TYPE_NV12_SEPARATE) {
    std::memcpy(in.sysMem[0].virAddr, nv12, static_cast<size_t>(y_size));
    std::memcpy(in.sysMem[1].virAddr, nv12 + y_size,
                static_cast<size_t>(uv_size));
    hbSysFlushMem(&in.sysMem[0], HB_SYS_MEM_CACHE_CLEAN);
    hbSysFlushMem(&in.sysMem[1], HB_SYS_MEM_CACHE_CLEAN);
  } else {
    std::memcpy(in.sysMem[0].virAddr, nv12,
                static_cast<size_t>(y_size + uv_size));
    hbSysFlushMem(&in.sysMem[0], HB_SYS_MEM_CACHE_CLEAN);
  }

  hbDNNTaskHandle_t task = nullptr;
  hbDNNInferCtrlParam ctrl;
  HB_DNN_INITIALIZE_INFER_CTRL_PARAM(&ctrl);
  int ret = hbDNNInfer(&task, &(bpu->output_tensor), &in, bpu->m_dnn_handle,
                       &ctrl);
  if (ret != 0) {
    std::fprintf(stderr, "hbDNNInfer failed ret=%d\n", ret);
    return ret;
  }
  ret = hbDNNWaitTaskDone(task, 0);
  hbDNNReleaseTask(task);
  return ret;
}

static void release_pose_bpu(bpu_module *bpu, hbDNNTensor out_tensors[2][kOutCount],
                             bool tensors_ok) {
  if (!bpu)
    return;
  if (tensors_ok) {
    for (int i = 0; i < 2; ++i)
      sp_deinit_bpu_tensor(out_tensors[i], kOutCount);
  }
  if (bpu->input_tensor.properties.tensorType == HB_DNN_IMG_TYPE_NV12_SEPARATE &&
      bpu->input_tensor.sysMem[1].virAddr) {
    hbSysFreeMem(&bpu->input_tensor.sysMem[1]);
    std::memset(&bpu->input_tensor.sysMem[1], 0,
                sizeof(bpu->input_tensor.sysMem[1]));
  }
  sp_release_bpu_module(bpu);
}

/* COCO-17 style skeleton (works with first 17 of 19-point models). */
static const int kSkeleton[][2] = {
    {5, 6},  {5, 7},  {7, 9},  {6, 8},  {8, 10}, {5, 11}, {6, 12},
    {11, 12},{11, 13},{13, 15},{12, 14},{14, 16},{0, 1},  {0, 2},
    {1, 3},  {2, 4},
};

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

static void scale_people_to_out(std::vector<PosePerson> &people, float sx,
                                float sy) {
  for (auto &p : people) {
    p.left *= sx;
    p.right *= sx;
    p.top *= sy;
    p.bottom *= sy;
    for (auto &k : p.kps) {
      k.x *= sx;
      k.y *= sy;
    }
  }
}

static void draw_pose(cv::Mat &bgr, const std::vector<PosePerson> &people) {
  for (const auto &p : people) {
    cv::rectangle(bgr, cv::Point(static_cast<int>(p.left), static_cast<int>(p.top)),
                  cv::Point(static_cast<int>(p.right), static_cast<int>(p.bottom)),
                  cv::Scalar(0, 200, 255), 2);
    const int n = static_cast<int>(p.kps.size());
    for (const auto &edge : kSkeleton) {
      int a = edge[0], b = edge[1];
      if (a >= n || b >= n)
        continue;
      if (p.kps[a].score < kKpsScoreTh || p.kps[b].score < kKpsScoreTh)
        continue;
      cv::line(bgr,
               cv::Point(static_cast<int>(p.kps[a].x),
                         static_cast<int>(p.kps[a].y)),
               cv::Point(static_cast<int>(p.kps[b].x),
                         static_cast<int>(p.kps[b].y)),
               cv::Scalar(0, 255, 0), 2);
    }
    for (const auto &k : p.kps) {
      if (k.score < kKpsScoreTh)
        continue;
      cv::circle(bgr, cv::Point(static_cast<int>(k.x), static_cast<int>(k.y)), 3,
                 cv::Scalar(0, 0, 255), -1);
    }
  }
}

static bool fill_kps_para(hbDNNTensor *kps_tensor, PoseKpsPara &para) {
  para.aligned_kps_dim.clear();
  para.kps_shifts.clear();
  const auto &prop = kps_tensor->properties;
  for (int i = 0; i < prop.alignedShape.numDimensions; ++i)
    para.aligned_kps_dim.push_back(prop.alignedShape.dimensionSize[i]);
  if (para.aligned_kps_dim.size() < 4)
    return false;
  /* Strides use aligned NHWC (e.g. 30,16,16,64); point count uses valid C. */
  para.kps_feat_height = para.aligned_kps_dim[1];
  para.kps_feat_width = para.aligned_kps_dim[2];
  int valid_c = prop.validShape.numDimensions >= 4
                    ? prop.validShape.dimensionSize[3]
                    : para.aligned_kps_dim[3];
  para.kps_points_number = valid_c >= 3 ? valid_c / 3 : 19;
  for (int i = 0; i < prop.shift.shiftLen; ++i)
    para.kps_shifts.push_back(
        static_cast<uint32_t>(static_cast<uint8_t>(prop.shift.shiftData[i])));
  if (para.kps_shifts.empty())
    para.kps_shifts.assign(static_cast<size_t>(para.kps_points_number * 3), 0);
  return true;
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
      std::getenv("VIGI_POSE_MODEL") ? std::getenv("VIGI_POSE_MODEL")
                                    : kDefaultModel;
  const int port =
      std::getenv("VIGI_PORT") ? std::atoi(std::getenv("VIGI_PORT")) : 8043;

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  std::fprintf(stderr,
               "vigi-encode-pose: SP+BPU+x264 %dx%d@%d br=%d model=%s -> :%d\n",
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
  hbDNNTensor out_tensors[2][kOutCount];
  int tensor_idx = 0;
  bool bpu_ready = false;
  PoseKpsPara kps_para{};

  const size_t nv12_sz = static_cast<size_t>(W) * static_cast<size_t>(H) * 3 / 2;
  const size_t model_nv12_sz =
      static_cast<size_t>(kModelW) * static_cast<size_t>(kModelH) * 3 / 2;
  std::vector<char> frame(nv12_sz);
  std::vector<uint8_t> model_nv12(model_nv12_sz);
  std::vector<uint8_t> out_nv12(nv12_sz);
  std::vector<PosePerson> last_people;

  const float sx = static_cast<float>(W) / static_cast<float>(kModelW);
  const float sy = static_cast<float>(H) / static_cast<float>(kModelH);

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

    if ((++cam_tick & 1) && !need_key && sent >= kPassthrough)
      continue;

    const uint8_t *encode_ptr =
        reinterpret_cast<const uint8_t *>(frame.data());

    if (sent < static_cast<uint64_t>(kPassthrough)) {
      if (sent + 1 == static_cast<uint64_t>(kPassthrough) && !bpu_ready) {
        std::fprintf(stderr, "loading BPU pose model %s\n", model_path);
        bpu = sp_init_bpu_module(model_path);
        if (!bpu) {
          std::fprintf(stderr, "sp_init_bpu_module failed — raw only\n");
        } else if (prepare_nv12_separate_input(bpu) != 0) {
          std::fprintf(stderr, "NV12_SEPARATE input alloc failed — raw only\n");
          sp_release_bpu_module(bpu);
          bpu = nullptr;
        } else {
          int ok = 1;
          for (int i = 0; i < 2; ++i) {
            if (sp_init_bpu_tensors(bpu, out_tensors[i]) != 0) {
              std::fprintf(stderr, "sp_init_bpu_tensors failed\n");
              ok = 0;
              break;
            }
          }
          if (ok && fill_kps_para(&out_tensors[0][kKpsIdx], kps_para)) {
            bpu_ready = true;
            std::fprintf(stderr,
                         "pose overlay armed (kps_points=%d feat=%dx%d "
                         "in_type=%d)\n",
                         kps_para.kps_points_number, kps_para.kps_feat_width,
                         kps_para.kps_feat_height,
                         bpu->input_tensor.properties.tensorType);
          } else {
            std::fprintf(stderr, "kps para init failed\n");
            release_pose_bpu(bpu, out_tensors, ok != 0);
            bpu = nullptr;
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
        cv::resize(bgr, resized, cv::Size(kModelW, kModelH), 0, 0,
                   cv::INTER_LINEAR);
        bgr_to_nv12(resized, model_nv12.data());

        bpu->output_tensor = &out_tensors[tensor_idx][0];
        if (pose_bpu_predict(bpu,
                             reinterpret_cast<char *>(model_nv12.data())) == 0) {
          for (int i = 0; i < kOutCount; ++i)
            hbSysFlushMem(&out_tensors[tensor_idx][i].sysMem[0],
                          HB_SYS_MEM_CACHE_INVALIDATE);
          std::vector<PosePerson> people;
          ParseBodyBoxes(&out_tensors[tensor_idx][kBodyBoxIdx], kBodyBoxIdx,
                         people, kBoxScoreTh);
          ParseBodyKps(&out_tensors[tensor_idx][kKpsIdx], kps_para, people);
          scale_people_to_out(people, sx, sy);
          last_people.swap(people);
          tensor_idx ^= 1;
        }
      }

      draw_pose(bgr, last_people);
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
                   "fps=%.1f encode_ms=%.1f people=%zu frames=%llu pose=%d\n",
                   n_log / sec, ms, last_people.size(),
                   static_cast<unsigned long long>(n), bpu_ready ? 1 : 0);
      t_log = now;
      n_log = 0;
    }
  }

  enc.close();
  if (bpu)
    release_pose_bpu(bpu, out_tensors, bpu_ready);
  sp_vio_close(cam);
  sp_release_vio_module(cam);
  if (sock >= 0)
    close(sock);
  return 0;
}
