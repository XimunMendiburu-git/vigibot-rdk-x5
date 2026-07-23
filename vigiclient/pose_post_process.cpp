#include "pose_post_process.hpp"

#include <cmath>
#include <cstring>

#include <dnn/hb_sys.h>

namespace {

struct BBoxF32 {
  float left;
  float top;
  float right;
  float bottom;
  float score;
  float class_label;
};

struct BBoxI16 {
  int16_t left;
  int16_t top;
  int16_t right;
  int16_t bottom;
  int8_t score;
  uint8_t class_label;
  int16_t padding[3];
};

inline float sigmoid(float x) { return 1.f / (1.f + std::exp(-x)); }

inline float get_float_by_int(int32_t value, uint32_t shift) {
  return static_cast<float>(value) / static_cast<float>(1u << shift);
}

} // namespace

int ParseBodyBoxes(hbDNNTensor *tensor, int branch_idx,
                   std::vector<PosePerson> &people, float score_th) {
  if (!tensor)
    return -1;
  hbSysFlushMem(&(tensor->sysMem[0]), HB_SYS_MEM_CACHE_INVALIDATE);
  auto *base = reinterpret_cast<uint8_t *>(tensor->sysMem[0].virAddr);
  if (!base)
    return -1;

  if (tensor->properties.tensorType == HB_DNN_TENSOR_TYPE_F32) {
    size_t item_size = sizeof(BBoxF32);
    float output_byte_size = *reinterpret_cast<float *>(base);
    uint32_t box_num = static_cast<uint32_t>(output_byte_size / item_size);
    auto *p_box = reinterpret_cast<BBoxF32 *>(
        reinterpret_cast<uintptr_t>(base) + item_size);
    for (uint32_t i = 0; i < box_num; ++i) {
      if (p_box[i].score < score_th)
        continue;
      PosePerson p;
      p.left = p_box[i].left;
      p.top = p_box[i].top;
      p.right = p_box[i].right;
      p.bottom = p_box[i].bottom;
      p.conf = p_box[i].score;
      (void)branch_idx;
      people.push_back(p);
    }
  } else {
    size_t item_size = sizeof(BBoxI16);
    uint16_t output_byte_size = *reinterpret_cast<uint16_t *>(base);
    uint16_t box_num = static_cast<uint16_t>(output_byte_size / item_size);
    auto *p_box = reinterpret_cast<BBoxI16 *>(
        reinterpret_cast<uintptr_t>(base) + item_size);
    for (uint32_t i = 0; i < box_num; ++i) {
      float score = static_cast<float>(p_box[i].score);
      if (score < score_th)
        continue;
      PosePerson p;
      p.left = p_box[i].left;
      p.top = p_box[i].top;
      p.right = p_box[i].right;
      p.bottom = p_box[i].bottom;
      p.conf = score;
      people.push_back(p);
    }
  }
  return 0;
}

int ParseBodyKps(hbDNNTensor *kps_tensor, const PoseKpsPara &para,
                 std::vector<PosePerson> &people) {
  if (!kps_tensor || para.aligned_kps_dim.size() < 4 ||
      para.kps_shifts.size() < static_cast<size_t>(para.kps_points_number * 3))
    return -1;
  if (people.empty())
    return 0;

  hbSysFlushMem(&(kps_tensor->sysMem[0]), HB_SYS_MEM_CACHE_INVALIDATE);
  auto *kps_feature =
      reinterpret_cast<int32_t *>(kps_tensor->sysMem[0].virAddr);
  if (!kps_feature)
    return -1;

  int feature_size = para.aligned_kps_dim[1] * para.aligned_kps_dim[2] *
                     para.aligned_kps_dim[3];
  int h_stride = para.aligned_kps_dim[2] * para.aligned_kps_dim[3];
  int w_stride = para.aligned_kps_dim[3];
  float pos_distance =
      para.kps_pos_distance * static_cast<float>(para.kps_feat_width);

  for (size_t box_id = 0; box_id < people.size(); ++box_id) {
    auto &body = people[box_id];
    float x1 = body.left;
    float y1 = body.top;
    float x2 = body.right;
    float y2 = body.bottom;
    float w = x2 - x1 + 1.f;
    float h = y2 - y1 + 1.f;
    if (w <= 1.f || h <= 1.f)
      continue;

    float scale_x = para.kps_feat_width / w;
    float scale_y = para.kps_feat_height / h;
    int32_t *begin = kps_feature + feature_size * static_cast<int>(box_id);

    body.kps.assign(static_cast<size_t>(para.kps_points_number), PosePoint{});
    for (int kps_id = 0; kps_id < para.kps_points_number; ++kps_id) {
      int max_w = 0;
      int max_h = 0;
      int max_score_before_shift = begin[kps_id];
      for (int hh = 0; hh < para.kps_feat_height; ++hh) {
        for (int ww = 0; ww < para.kps_feat_width; ++ww) {
          int32_t *cell = begin + hh * h_stride + ww * w_stride;
          if (cell[kps_id] > max_score_before_shift) {
            max_w = ww;
            max_h = hh;
            max_score_before_shift = cell[kps_id];
          }
        }
      }

      float max_score =
          get_float_by_int(max_score_before_shift, para.kps_shifts[kps_id]);
      int32_t *best = begin + max_h * h_stride + max_w * w_stride;
      int x_idx = 2 * kps_id + para.kps_points_number;
      int y_idx = 2 * kps_id + para.kps_points_number + 1;
      float fp_delta_x =
          get_float_by_int(best[x_idx], para.kps_shifts[x_idx]) * pos_distance;
      float fp_delta_y =
          get_float_by_int(best[y_idx], para.kps_shifts[y_idx]) * pos_distance;

      PosePoint pt;
      pt.x = (max_w + fp_delta_x + 0.46875f + para.kps_anchor_param) / scale_x +
             x1;
      pt.y = (max_h + fp_delta_y + 0.46875f + para.kps_anchor_param) / scale_y +
             y1;
      pt.score = sigmoid(max_score);
      body.kps[static_cast<size_t>(kps_id)] = pt;
    }
  }
  return 0;
}
