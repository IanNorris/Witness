# ONNX Detection Models

This directory stores ONNX model files for object detection. Models are not committed to the repository due to their size.

## Quick Start

Download and export the default model (nano):

```bash
pip install ultralytics
python scripts/download-models.py
```

This downloads `yolo26n.pt` from Ultralytics and exports it to `yolo26n.onnx`.

## Download All Variants

```bash
python scripts/download-models.py --all
```

Or specific variants:

```bash
python scripts/download-models.py --variants n s m
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

## License

YOLO26 model weights are licensed under AGPL-3.0 by Ultralytics, compatible with Witness's GPLv3 license.
