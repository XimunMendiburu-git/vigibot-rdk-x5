#ifndef VIGI_POSE_POST_PROCESS_HPP
#define VIGI_POSE_POST_PROCESS_HPP

#include <cstdint>
#include <vector>

#include <dnn/hb_dnn.h>

struct PosePoint {
  float x = 0;
  float y = 0;
  float score = 0;
};

struct PosePerson {
  float left = 0;
  float top = 0;
  float right = 0;
  float bottom = 0;
  float conf = 0;
  std::vector<PosePoint> kps; /* typically 17 or 19 */
};

struct PoseKpsPara {
  int kps_points_number = 19;
  float kps_pos_distance = 0.1f;
  float kps_anchor_param = -0.46875f;
  int kps_feat_width = 16;
  int kps_feat_height = 16;
  std::vector<int> aligned_kps_dim; /* N,H,W,C aligned */
  std::vector<uint32_t> kps_shifts;
};

/* Parse body boxes from FasterRCNN branch (float or int16 packed). */
int ParseBodyBoxes(hbDNNTensor *tensor, int branch_idx,
                   std::vector<PosePerson> &people, float score_th);

/* Fill people[].kps from kps branch using body boxes already parsed. */
int ParseBodyKps(hbDNNTensor *kps_tensor, const PoseKpsPara &para,
                 std::vector<PosePerson> &people);

#endif
