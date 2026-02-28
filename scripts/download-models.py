#!/usr/bin/env python3
"""Download YOLO26 ONNX models for Witness object detection.

Downloads pretrained weights from HuggingFace and exports to ONNX format.
Requires: pip install ultralytics
"""

import argparse
import os
import sys

MODELS = {
    "n": {"filename": "yolo26n.onnx", "pt": "yolo26n.pt", "description": "Nano (~10MB, 39ms CPU)"},
    "s": {"filename": "yolo26s.onnx", "pt": "yolo26s.pt", "description": "Small (~20MB, 87ms CPU)"},
    "m": {"filename": "yolo26m.onnx", "pt": "yolo26m.pt", "description": "Medium (~40MB, 220ms CPU)"},
    "l": {"filename": "yolo26l.onnx", "pt": "yolo26l.pt", "description": "Large (~80MB, 286ms CPU)"},
    "x": {"filename": "yolo26x.onnx", "pt": "yolo26x.pt", "description": "XLarge (~130MB, 526ms CPU)"},
}


def export_model(variant, output_dir):
    """Download .pt weights via ultralytics and export to ONNX."""
    try:
        from ultralytics import YOLO
    except ImportError:
        print("  Error: ultralytics package required. Install with: pip install ultralytics")
        return False

    info = MODELS[variant]
    onnx_path = os.path.join(output_dir, info["filename"])

    if os.path.exists(onnx_path):
        print(f"  {info['filename']} already exists, skipping.")
        return True

    print(f"  Loading {info['pt']} (downloads automatically if needed)...")
    try:
        model = YOLO(info["pt"])
        print(f"  Exporting to ONNX (end-to-end, NMS-free, 640x640)...")
        model.export(format="onnx", imgsz=640)

        # ultralytics exports next to the .pt file
        exported = info["pt"].replace(".pt", ".onnx")
        if os.path.exists(exported):
            os.rename(exported, onnx_path)
            size_mb = os.path.getsize(onnx_path) / (1024 * 1024)
            print(f"  Saved: {onnx_path} ({size_mb:.1f} MB)")

            # Clean up .pt file
            if os.path.exists(info["pt"]):
                os.remove(info["pt"])
            return True
        else:
            print(f"  Export produced no output file.")
            return False

    except Exception as e:
        print(f"  Export failed: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description="Download YOLO26 ONNX models for Witness")
    parser.add_argument("--variants", nargs="+", choices=list(MODELS.keys()), default=["n"],
                        help="Model variants to download (default: n)")
    parser.add_argument("--all", action="store_true", help="Download all model variants")
    parser.add_argument("--output", default=None, help="Output directory (default: models/ at repo root)")
    args = parser.parse_args()

    if args.all:
        args.variants = list(MODELS.keys())

    # Default output: models/ directory at repo root
    if args.output:
        output_dir = args.output
    else:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        output_dir = os.path.join(os.path.dirname(script_dir), "models")

    os.makedirs(output_dir, exist_ok=True)

    print(f"Exporting YOLO26 models to: {output_dir}")
    print(f"Variants: {', '.join(args.variants)}")
    print()

    success = 0
    failed = 0
    for variant in args.variants:
        info = MODELS[variant]
        print(f"[{variant}] {info['description']}")
        if export_model(variant, output_dir):
            success += 1
        else:
            failed += 1
        print()

    print(f"Done: {success} exported, {failed} failed.")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
