/*
 * Vigibot RDK X5 — low-latency software H.264 encoder
 *   SP camera (NV12) -> libx264 Constrained Baseline -> TCP 127.0.0.1:8043
 *   --stdin NV12 pipe  -> libx264 Constrained Baseline -> TCP 127.0.0.1:8043
 *
 * Usage:
 *   vigi-encode-x264 WIDTH HEIGHT FPS [BITRATE_BPS]
 *   vigi-encode-x264 --stdin WIDTH HEIGHT FPS [BITRATE_BPS]
 */
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include <x264.h>
#include "sp_vio.h"
}

static std::atomic<bool> g_run{true};
static void on_sig(int) { g_run = false; }

struct StreamParams {
  int W = 640;
  int H = 480;
  int fps = 8;
  int br = 100000;
  int port = 8043;
  bool stdin_mode = false;
};

static StreamParams parse_params(int argc, char **argv) {
  StreamParams p{};
  int base = 1;
  if (argc >= 2 && std::strcmp(argv[1], "--stdin") == 0) {
    p.stdin_mode = true;
    base = 2;
  }
  if (argc < base + 3) {
    std::fprintf(stderr,
                 "usage: %s [--stdin] WIDTH HEIGHT FPS [BITRATE_BPS]\n",
                 argv[0]);
    std::exit(1);
  }

  p.W = std::atoi(argv[base]);
  p.H = std::atoi(argv[base + 1]);
  p.fps = std::atoi(argv[base + 2]);
  p.br = argc > base + 3 ? std::atoi(argv[base + 3]) : 100000;

  if (p.W <= 0)
    p.W = 640;
  if (p.H <= 0)
    p.H = 480;
  if (p.W < 640)
    p.W = 640;
  if (p.H < 480)
    p.H = 480;
  p.W &= ~1;
  p.H &= ~1;
  if (p.fps < 5)
    p.fps = 8;
  if (p.fps > 8)
    p.fps = 8;
  if (p.br < 70000)
    p.br = 70000;
  if (p.br > 100000)
    p.br = 100000;

  p.port = std::getenv("VIGI_PORT") ? std::atoi(std::getenv("VIGI_PORT")) : 8043;
  return p;
}

static bool read_full(int fd, void *buf, size_t n) {
  auto *p = static_cast<uint8_t *>(buf);
  while (n > 0 && g_run) {
    ssize_t r = ::read(fd, p, n);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (r == 0)
      return false;
    p += static_cast<size_t>(r);
    n -= static_cast<size_t>(r);
  }
  return n == 0;
}

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
    /* Small buffer so backpressure shows up instead of hiding old frames. */
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
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return false;
      return false;
    }
    if (w == 0)
      return false;
    b += static_cast<size_t>(w);
    n -= static_cast<size_t>(w);
  }
  return true;
}

/* Vigibot Node splitter only cuts on 00 00 00 01 — never emit 3-byte start codes. */
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
    /* ~1s GOP */
    param.i_keyint_max = fps;
    param.i_keyint_min = fps;
    param.b_repeat_headers = 1;
    param.b_annexb = 1;
    param.i_bframe = 0;
    param.b_cabac = 0;
    /* CRF + tight VBV: better looking frames than ABR at the same cap. */
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
    /* Force level 3.1 (common for 640x480 browser streams). */
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

  /* Returns bytes sent, 0 if nothing, -1 hard error, -2 backpressure. */
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

int main(int argc, char **argv) {
  StreamParams sp = parse_params(argc, argv);

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  std::fprintf(stderr,
               "vigi-encode-x264: %s libx264 CB %dx%d@%d br=%d -> :%d\n",
               sp.stdin_mode ? "stdin NV12 +" : "SP cam +", sp.W, sp.H, sp.fps,
               sp.br, sp.port);

  X264Enc enc;
  if (!enc.open(sp.W, sp.H, sp.fps, sp.br)) {
    std::fprintf(stderr, "libx264 open failed\n");
    return 1;
  }
  std::fprintf(stderr, "libx264 ready (baseline / veryfast / zerolatency / crf)\n");

  const size_t nv12_sz =
      static_cast<size_t>(sp.W) * static_cast<size_t>(sp.H) * 3 / 2;
  std::vector<char> frame(nv12_sz);
  int64_t pts = 0;
  uint64_t n = 0;
  int sock = -1;
  bool need_key = true;
  int cam_tick = 0;
  auto t_log = std::chrono::steady_clock::now();
  uint64_t n_log = 0;

  void *cam = nullptr;
  if (!sp.stdin_mode) {
    cam = sp_init_vio_module();
    if (!cam) {
      std::fprintf(stderr, "sp_init_vio_module failed\n");
      return 1;
    }

    sp_sensors_parameters sens{};
    sens.fps = 15;
    sens.raw_width = -1;
    sens.raw_height = -1;
    int32_t widths[1] = {sp.W};
    int32_t heights[1] = {sp.H};
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
      std::fprintf(stderr, "sp_open_camera_v2 failed permanently\n");
      sp_release_vio_module(cam);
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
  }

  while (g_run) {
    if (sock < 0) {
      std::fprintf(stderr, "connecting tcp://127.0.0.1:%d ...\n", sp.port);
      sock = connect_8043(sp.port);
      if (sock < 0)
        break;
      std::fprintf(stderr, "connected tcp://127.0.0.1:%d\n", sp.port);
      need_key = true;
    }

    auto t0 = std::chrono::steady_clock::now();
    if (sp.stdin_mode) {
      if (!read_full(STDIN_FILENO, frame.data(), nv12_sz))
        break;
    } else {
      if (sp_vio_get_frame(cam, frame.data(), sp.W, sp.H, 2000) != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      if ((++cam_tick & 1) && !need_key)
        continue;
    }

    int got = enc.encode_nv12(reinterpret_cast<uint8_t *>(frame.data()), pts++,
                              sock, need_key);
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

    auto now = std::chrono::steady_clock::now();
    if (now - t_log >= std::chrono::seconds(2)) {
      double sec = std::chrono::duration<double>(now - t_log).count();
      double ms = std::chrono::duration<double, std::milli>(now - t0).count();
      std::fprintf(stderr, "fps=%.1f encode_ms=%.1f frames=%llu\n",
                   n_log / sec, ms, static_cast<unsigned long long>(n));
      t_log = now;
      n_log = 0;
    }
  }

  enc.close();
  if (cam) {
    sp_vio_close(cam);
    sp_release_vio_module(cam);
  }
  if (sock >= 0)
    close(sock);
  return 0;
}
