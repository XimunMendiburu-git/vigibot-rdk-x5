# Agricultural Detection on RDK X5 — Recommended Models & Pipeline

Design notes for **pest / plant-disease detection** on the D-Robotics **RDK X5**, intended for a future Vigibot video source (BPU overlay → libx264 live stream).

This document is the **recommended architecture**. The live source today still uses the stock board model documented in [yolo-source.md](./yolo-source.md) (`yolov5s_v7` COCO). Migrating to a fine-tuned YOLOv8n/v11n (and optionally a 2-stage classifier) is the next AI step.

---

## 1. Recommended detector: YOLOv8n or YOLOv11n

For object detection on RDK X5, the **YOLO Nano / Small** family is the practical sweet spot.

| Choice | Role |
|--------|------|
| **YOLOv8n** or **YOLOv11n** | Direct detection: bounding box + class |
| Fine-tune | Agricultural dataset (pests, lesions, leaves) before BPU conversion |

### Why YOLO Nano for pests

Pest detection is a **small / fine object** problem (aphid, caterpillar on a leaf). YOLO’s feature pyramid keeps **high-resolution detail** that coarser detectors tend to lose. Nano variants keep enough capacity for field use while staying BPU-friendly after INT8 quantization.

### Expected RDK X5 performance (order of magnitude)

After conversion to the BPU binary format (INT8) with the D-Robotics / Hobot NN toolchain, **YOLOv8n** typically runs in the **~60–100 FPS** class on X5 while using little memory — enough headroom to share the CPU with capture, overlay drawing, and H.264 encode (see [video-codecs-and-encoders-guide.md](./video-codecs-and-encoders-guide.md)).

Exact FPS depends on input size (e.g. 640×640), NMS load, and whether you infer every frame (`INFER_EVERY`).

**Not recommended as first choice for this use case:** large YOLO variants (l/x) — accuracy gains rarely justify BPU latency and memory on a teleop robot.

---

## 2. Best strategy when you need fine-grained IDs: Leaf-First 2-stage

If the product must identify **plant species**, **disease**, and **exact pest type** among many classes with high precision, a single detector with hundreds of tiny classes is often weaker than a **hybrid pipeline**:

```text
MIPI camera (RDK X5)
  │
  ├──► Stage 1: YOLOv8n / YOLOv11n (BPU)
  │         Detect & crop leaves / insects / lesions
  │
  └──► Stage 2: MobileNetV3-Small or ResNet-18 (BPU)
            Classify pest / disease on each crop
```

| Stage | Model | Job |
|-------|--------|-----|
| **1 — Detection** | YOLOv8n / YOLOv11n | Find regions of interest (boxes around insects or leaf lesions) |
| **2 — Classification** | MobileNetV3-Small or ResNet-18 | Fine ID among many classes on the cropped patch |

### Why this is SOTA-style on embedded boards

- Stage 1 stays fast and stable (few geometric classes: leaf, pest, lesion, …).
- Stage 2 sees a **zoomed patch**, which helps species-level accuracy.
- Both stages map cleanly to **INT8 BPU** models; you can run stage 2 only on the top-K boxes to bound latency.

### Fit with Vigibot

Same pattern as today’s source 1: NV12 → BPU → draw boxes/labels → **libx264** → TCP. Stage 2 adds one or more classifier forwards per detection; use `INFER_EVERY` and a max-crops-per-frame cap so the live stream stays responsive.

---

## 3. Reference datasets for fine-tuning

Train (or fine-tune) on a PC **before** converting for the X5 BPU.

### Pests

| Dataset | Notes |
|---------|--------|
| **[IP102](https://github.com/xpwu95/IP102)** | ~75k+ images, **102** pest insect classes — strong starting point |
| **AgPest** | Agricultural pest sets (use when domain matches your crops/region) |

### Plants & diseases

| Dataset | Notes |
|---------|--------|
| **[PlantDoc](https://github.com/pratikkayal/PlantDoc-Dataset)** | Real-field conditions — closer to robot camera noise |
| **[PlantVillage](https://plantvillage.psu.edu/)** | Large, clean lab-style images — good pretrain, weaker domain match outdoors |

**Practical tip:** pretrain or warm-start on PlantVillage / IP102, then fine-tune on a small **on-robot** set (same IMX219, lighting, distance) so the deployed model matches Vigibot frames.

---

## 4. Technical deployment on RDK X5

D-Robotics provides the path from **ONNX → quantized BPU binary** (conventionally `.bin` on current OpenExplorer / hobot stacks; follow the toolchain version shipped with your image).

```text
PyTorch / Ultralytics  →  ONNX (e.g. yolov8n.onnx)
        →  Hobot NN Toolchain (INT8 calibration)
        →  BPU model (.bin)
        →  hobot_dnn / sp_bpu inference on device
```

| Step | What you do |
|------|-------------|
| **Train & export** | Ultralytics (or equivalent) → export **ONNX** |
| **INT8 quantize** | Hobot / D-Robotics NN toolchain on a PC — X5 BPU is built around **INT8** throughput (on the order of **10 TOPS**) |
| **Run on board** | C++ or Python via **`hobot_dnn`** / `sp_bpu_*` with full BPU acceleration |
| **Post-process** | YOLO decode + NMS (as in `yolov5_post_process` today); classifier softmax / top-1 for stage 2 |
| **Live path** | Overlay on NV12/BGR → encode with **libx264** for browser-safe Baseline (HW Wave521 is optional only if the client accepts it) |

### Checklist before swapping the Vigibot YOLO binary

1. Confirm input layout (**NV12** vs **NV12_SEPARATE**) matches the compiled model.  
2. Align **HBRT / model compiler** versions to avoid runtime surprises.  
3. Dump a few annotated frames offline, then enable the live source.  
4. Keep stream-first / passthrough behavior so Vigibot gets frames while the model loads ([yolo-source.md](./yolo-source.md) §4).

---

## 5. Relation to the current POC

| Item | Today | Target (this doc) |
|------|--------|-------------------|
| Model | Stock `yolov5s_v7_640x640_nv12.bin` (COCO) | Fine-tuned **YOLOv8n / YOLOv11n** (agri) |
| Classes | Generic COCO | Pests / leaves / lesions (+ optional stage-2 IDs) |
| Pipeline | Single-shot detect + draw | Optional **2-stage** detect → classify |
| Encode | libx264 C++ | Unchanged |

---

## 6. Takeaway

- **Detector of choice on X5 for agri teleop:** YOLOv8n or YOLOv11n, INT8 on BPU.  
- **When class count and precision matter:** Leaf-First **detect → crop → MobileNetV3/ResNet-18**.  
- **Data:** IP102 / AgPest + PlantDoc / PlantVillage, then domain fine-tune on robot images.  
- **Deploy:** Ultralytics → ONNX → Hobot INT8 toolchain → `hobot_dnn` on RDK X5 → same Vigibot overlay encode path.
