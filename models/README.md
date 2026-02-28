# ONNX Detection Models

This directory stores ONNX model files for object detection. Models are not committed to the repository due to their size.

## Download Models

Run the download script to fetch all YOLO26 model variants from HuggingFace:

```bash
python scripts/download-models.py
```

If you need authentication (e.g., for rate limits):

```bash
python scripts/download-models.py --token YOUR_HUGGINGFACE_TOKEN
```

To download only specific variants:

```bash
python scripts/download-models.py --variants n s
```

## Available Models

| Model | Size | mAP (COCO) | CPU Inference | Best For |
|-------|------|------------|---------------|----------|
| yolo26n.onnx | ~6 MB | 40.9 | ~39ms | Multi-camera, low-power (default) |
| yolo26s.onnx | ~20 MB | 48.6 | ~87ms | Balanced accuracy/speed |
| yolo26m.onnx | ~40 MB | 53.1 | ~220ms | Dedicated GPU setups |
| yolo26l.onnx | ~80 MB | 55.0 | ~286ms | High accuracy, GPU required |
| yolo26x.onnx | ~130 MB | 57.5 | ~526ms | Maximum accuracy, powerful GPU |

All models detect 80 COCO object classes including person, car, truck, cat, dog, bird, and more.

## License

YOLO26 model weights are licensed under AGPL-3.0 by Ultralytics, compatible with Witness's GPLv3 license.
