#include "yolov5_post_process.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <dnn/hb_sys.h>

namespace {

struct PTQYolo5Config {
  std::vector<int> strides;
  std::vector<std::vector<std::pair<double, double>>> anchors_table;
  int class_num;
  std::vector<std::string> class_names;
};

const PTQYolo5Config kCfg = {
    {8, 16, 32},
    {{{10, 13}, {16, 30}, {33, 23}},
     {{30, 61}, {62, 45}, {59, 119}},
     {{116, 90}, {156, 198}, {373, 326}}},
    80,
    {"person",        "bicycle",      "car",
     "motorcycle",    "airplane",     "bus",
     "train",         "truck",        "boat",
     "traffic light", "fire hydrant", "stop sign",
     "parking meter", "bench",        "bird",
     "cat",           "dog",          "horse",
     "sheep",         "cow",          "elephant",
     "bear",          "zebra",        "giraffe",
     "backpack",      "umbrella",     "handbag",
     "tie",           "suitcase",     "frisbee",
     "skis",          "snowboard",    "sports ball",
     "kite",          "baseball bat", "baseball glove",
     "skateboard",    "surfboard",    "tennis racket",
     "bottle",        "wine glass",   "cup",
     "fork",          "knife",        "spoon",
     "bowl",          "banana",       "apple",
     "sandwich",      "orange",       "broccoli",
     "carrot",        "hot dog",      "pizza",
     "donut",         "cake",         "chair",
     "couch",         "potted plant", "bed",
     "dining table",  "toilet",       "tv",
     "laptop",        "mouse",        "remote",
     "keyboard",      "cell phone",   "microwave",
     "oven",          "toaster",      "sink",
     "refrigerator",  "book",         "clock",
     "vase",          "scissors",     "teddy bear",
     "hair drier",    "toothbrush"}};

const float kScoreTh = 0.4f;

template <class It>
size_t argmax(It first, It last) {
  return static_cast<size_t>(std::distance(first, std::max_element(first, last)));
}

int get_tensor_hw(hbDNNTensor *tensor, int *height, int *width) {
  int h_index = 0;
  int w_index = 0;
  if (tensor->properties.tensorLayout == HB_DNN_LAYOUT_NHWC) {
    h_index = 1;
    w_index = 2;
  } else if (tensor->properties.tensorLayout == HB_DNN_LAYOUT_NCHW) {
    h_index = 2;
    w_index = 3;
  } else {
    return -1;
  }
  *height = tensor->properties.validShape.dimensionSize[h_index];
  *width = tensor->properties.validShape.dimensionSize[w_index];
  return 0;
}

} // namespace

void ParseTensor(hbDNNTensor *tensor, int layer,
                 std::vector<YoloV5Result> &results,
                 bpu_image_info_t &image_info) {
  hbSysFlushMem(&(tensor->sysMem[0]), HB_SYS_MEM_CACHE_INVALIDATE);
  int num_classes = kCfg.class_num;
  int stride = kCfg.strides[layer];
  int num_pred = kCfg.class_num + 4 + 1;
  auto &anchors = kCfg.anchors_table[layer];

  double h_ratio = image_info.m_model_h * 1.0 / image_info.m_ori_height;
  double w_ratio = image_info.m_model_w * 1.0 / image_info.m_ori_width;

  int height = 0, width = 0;
  if (get_tensor_hw(tensor, &height, &width) != 0)
    return;

  int anchor_num = static_cast<int>(anchors.size());
  auto *data = reinterpret_cast<float *>(tensor->sysMem[0].virAddr);
  for (int h = 0; h < height; h++) {
    for (int w = 0; w < width; w++) {
      for (int k = 0; k < anchor_num; k++) {
        double anchor_x = anchors[k].first;
        double anchor_y = anchors[k].second;
        float *cur_data = data + k * num_pred;
        float objness = cur_data[4];
        int id = static_cast<int>(argmax(cur_data + 5, cur_data + 5 + num_classes));
        double confidence =
            (1.0 / (1.0 + std::exp(-objness))) *
            (1.0 / (1.0 + std::exp(-cur_data[id + 5])));
        if (confidence < kScoreTh)
          continue;

        float center_x = cur_data[0];
        float center_y = cur_data[1];
        float scale_x = cur_data[2];
        float scale_y = cur_data[3];

        double box_center_x =
            ((1.0 / (1.0 + std::exp(-center_x))) * 2 - 0.5 + w) * stride;
        double box_center_y =
            ((1.0 / (1.0 + std::exp(-center_y))) * 2 - 0.5 + h) * stride;
        double box_scale_x =
            std::pow((1.0 / (1.0 + std::exp(-scale_x))) * 2, 2) * anchor_x;
        double box_scale_y =
            std::pow((1.0 / (1.0 + std::exp(-scale_y))) * 2, 2) * anchor_y;

        double xmin = box_center_x - box_scale_x / 2.0;
        double ymin = box_center_y - box_scale_y / 2.0;
        double xmax = box_center_x + box_scale_x / 2.0;
        double ymax = box_center_y + box_scale_y / 2.0;

        double w_padding =
            (image_info.m_model_w - w_ratio * image_info.m_ori_width) / 2.0;
        double h_padding =
            (image_info.m_model_h - h_ratio * image_info.m_ori_height) / 2.0;

        double xmin_org = (xmin - w_padding) / w_ratio;
        double xmax_org = (xmax - w_padding) / w_ratio;
        double ymin_org = (ymin - h_padding) / h_ratio;
        double ymax_org = (ymax - h_padding) / h_ratio;

        if (xmax <= 0 || ymax <= 0 || xmin > xmax || ymin > ymax)
          continue;

        xmin_org = std::max(xmin_org, 0.0);
        xmax_org = std::min(xmax_org, image_info.m_ori_width - 1.0);
        ymin_org = std::max(ymin_org, 0.0);
        ymax_org = std::min(ymax_org, image_info.m_ori_height - 1.0);

        results.emplace_back(
            id, static_cast<float>(xmin_org), static_cast<float>(ymin_org),
            static_cast<float>(xmax_org), static_cast<float>(ymax_org),
            static_cast<float>(confidence), kCfg.class_names[id]);
      }
      data = data + num_pred * anchors.size();
    }
  }
}

void yolo5_nms(std::vector<YoloV5Result> &input, float iou_threshold, int top_k,
               std::vector<YoloV5Result> &result, bool suppress) {
  std::stable_sort(input.begin(), input.end(), std::greater<YoloV5Result>());
  std::vector<bool> skip(input.size(), false);
  std::vector<float> areas;
  areas.reserve(input.size());
  for (auto &box : input) {
    areas.push_back((box.xmax - box.xmin) * (box.ymax - box.ymin));
  }

  int count = 0;
  for (size_t i = 0; count < top_k && i < skip.size(); i++) {
    if (skip[i])
      continue;
    skip[i] = true;
    ++count;

    for (size_t j = i + 1; j < skip.size(); ++j) {
      if (skip[j])
        continue;
      if (!suppress && input[i].id != input[j].id)
        continue;

      float xx1 = std::max(input[i].xmin, input[j].xmin);
      float yy1 = std::max(input[i].ymin, input[j].ymin);
      float xx2 = std::min(input[i].xmax, input[j].xmax);
      float yy2 = std::min(input[i].ymax, input[j].ymax);
      if (xx2 > xx1 && yy2 > yy1) {
        float inter = (xx2 - xx1) * (yy2 - yy1);
        float iou = inter / (areas[j] + areas[i] - inter);
        if (iou > iou_threshold)
          skip[j] = true;
      }
    }
    result.push_back(input[i]);
  }
}
