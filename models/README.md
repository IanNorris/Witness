# ONNX Models

This directory stores ONNX model files for object detection and face recognition. Models are not committed to the repository due to their size.

## Quick Start

Download the default detection model (nano) and face recognition model:

```bash
pip install ultralytics
python scripts/download-models.py --face
```

This downloads `yolo26n.onnx` (object detection) and `face_recognition.onnx` (MobileFaceNet).

## Download All Variants

```bash
python scripts/download-models.py --all
```

Or specific variants:

```bash
python scripts/download-models.py --variants n s m --face
```

## Available Models

| Model | Size | mAP (COCO) | CPU Inference | Best For |
|-------|------|------------|---------------|----------|
| yolo26n.onnx | ~10 MB | 40.9 | ~39ms | Multi-camera, low-power (default) |
| yolo26s.onnx | ~20 MB | 48.6 | ~87ms | Balanced accuracy/speed |
| yolo26m.onnx | ~40 MB | 53.1 | ~220ms | Dedicated GPU setups |
| yolo26l.onnx | ~80 MB | 55.0 | ~286ms | High accuracy, GPU required |
| yolo26x.onnx | ~130 MB | 57.5 | ~526ms | Maximum accuracy, powerful GPU |

All models detect 80 COCO object classes including person, car, truck, cat, dog, bird, and more.

## Server Configuration

Set these in the Witness database settings to enable detection:

| Setting | Value | Description |
|---------|-------|-------------|
| `detection_backend` | `onnx` | Enable ONNX detection |
| `detection_model_path` | *(optional)* | Path to .onnx file (default: `models/yolo26n.onnx` next to exe) |
| `detection_confidence` | `0.5` | Minimum confidence threshold (0.0-1.0) |
| `detection_provider` | `cpu` or `gpu` | Execution provider (gpu = CUDA) |

## Face Detection Model

YuNet is used for face detection, running on person crops from YOLO to detect faces.

| Model | Size | License | Accuracy (WIDER FACE) |
|-------|------|---------|-----------------------|
| face_detection_yunet_2023mar.onnx | ~230 KB | MIT | 88.4% Easy, 86.6% Medium, 75.0% Hard |

Download from [OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/master/models/face_detection_yunet).
Uses OpenCV's built-in `cv::FaceDetectorYN` — no custom ONNX loading needed.

| Setting | Value | Description |
|---------|-------|-------------|
| `face_detection_enabled` | `1` | Enable face detection |
| `face_detection_confidence` | `0.7` | Minimum confidence threshold (0.0-1.0) |

## Face Recognition Model

MobileFaceNet (w600k_mbf) generates 512-dim embeddings for face matching against known identities.

| Model | Size | Embedding | LFW Accuracy | Source |
|-------|------|-----------|--------------|--------|
| face_recognition.onnx | ~13 MB | 512-dim | 99.7% | InsightFace buffalo_sc |

Input: `[1, 3, 112, 112]` float (aligned face, BGR→RGB, normalized to [0,1]).
Output: `[1, 512]` float (L2-normalized embedding).

Download automatically:
```bash
python scripts/download-models.py --face
```

| Setting | Value | Description |
|---------|-------|-------------|
| `face_recognition_enabled` | `1` | Enable face recognition |
| `face_recognition_model_path` | *(optional)* | Path to .onnx file (default: `models/face_recognition.onnx` next to exe) |
| `face_recognition_confidence` | `0.5` | Cosine similarity threshold for identity match (0.0-1.0) |

## License

YOLO26 model weights are licensed under AGPL-3.0 by Ultralytics, compatible with Witness's GPLv3 license.
YuNet model is licensed under MIT by OpenCV Zoo — no restrictions for any use.
MobileFaceNet (w600k_mbf) from InsightFace is for **non-commercial research purposes only**.
