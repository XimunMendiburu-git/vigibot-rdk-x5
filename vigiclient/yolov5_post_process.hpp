#ifndef VIGI_YOLOV5_POST_PROCESS_HPP
#define VIGI_YOLOV5_POST_PROCESS_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sp_bpu.h"
#include <dnn/hb_dnn.h>

struct YoloV5Result {
  int id = 0;
  float xmin = 0;
  float ymin = 0;
  float xmax = 0;
  float ymax = 0;
  float score = 0;
  std::string class_name;

  YoloV5Result(int id_, float xmin_, float ymin_, float xmax_, float ymax_,
               float score_, std::string class_name_)
      : id(id_), xmin(xmin_), ymin(ymin_), xmax(xmax_), ymax(ymax_),
        score(score_), class_name(std::move(class_name_)) {}

  friend bool operator>(const YoloV5Result &lhs, const YoloV5Result &rhs) {
    return lhs.score > rhs.score;
  }
};

void ParseTensor(hbDNNTensor *tensor, int layer,
                 std::vector<YoloV5Result> &results,
                 bpu_image_info_t &image_info);

void yolo5_nms(std::vector<YoloV5Result> &input, float iou_threshold, int top_k,
               std::vector<YoloV5Result> &result, bool suppress);

#endif
