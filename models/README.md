# ONNX Models

This directory stores ONNX model files for object detection and face recognition. Models are not committed to the repository due to their size.

It also holds the optional YAMNet audio-classification research model. Download
it with `python scripts/download-models.py --audio`; the downloader pins and
verifies the exact ONNX conversion and AudioSet class map. Then evaluate one or
more camera clips without retaining decoded audio:

```bash
pip install numpy onnxruntime
python scripts/audio_intelligence_eval.py clip.mp4 --json audio-report.json
```

`ffmpeg` and `ffprobe` must be on `PATH`, or supplied with the corresponding
command-line options. The evaluator records model hashes, provider/thread
settings, whole-clip and per-window timings, grouped trigger scores, signal
levels, and packet-size correlations in the report.

YAMNet is Apache-2.0 and the AudioSet labels are CC BY 4.0. The pinned ONNX
artifact is a straight `tf2onnx` conversion of Google YAMNet v1 with unchanged
weights; provenance is documented by its publisher at
https://huggingface.co/audiomagic/yamnet-onnx.

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

## Audio Intelligence Model

YAMNet classifies mono 16 kHz audio into AudioSet classes. Witness maps those
raw scores into a small set of product-facing sound families and stores only
event metadata. Install the pinned, hash-verified model and class map with:

```bash
python scripts/download-models.py --audio
```

Audio intelligence is disabled by default and enabled per camera in the admin
UI. Optional global overrides are `audio_intelligence_model_path` and
`audio_intelligence_class_map_path`; otherwise the server looks in its adjacent
`models` directory.

## License

YOLO26 model weights are licensed under AGPL-3.0 by Ultralytics, compatible with Witness's GPLv3 license.
YuNet model is licensed under MIT by OpenCV Zoo — no restrictions for any use.
MobileFaceNet (w600k_mbf) from InsightFace is for **non-commercial research purposes only**.
YAMNet is distributed by TensorFlow Models under Apache-2.0; AudioSet labels are
descriptive metadata rather than retained training media.
