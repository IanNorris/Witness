#!/usr/bin/env python3
"""Download YOLO26 ONNX models from HuggingFace for Witness object detection."""

import argparse
import os
import sys
import urllib.request
import urllib.error

MODELS = {
    "n": {"filename": "yolo26n.onnx", "description": "Nano (~6MB, 39ms CPU)"},
    "s": {"filename": "yolo26s.onnx", "description": "Small (~20MB, 87ms CPU)"},
    "m": {"filename": "yolo26m.onnx", "description": "Medium (~40MB, 220ms CPU)"},
    "l": {"filename": "yolo26l.onnx", "description": "Large (~80MB, 286ms CPU)"},
    "x": {"filename": "yolo26x.onnx", "description": "XLarge (~130MB, 526ms CPU)"},
}

# HuggingFace model repo URL pattern
HF_BASE_URL = "https://huggingface.co/Ultralytics/YOLO26/resolve/main"


def download_model(variant, output_dir, token=None):
    info = MODELS[variant]
    filename = info["filename"]
    output_path = os.path.join(output_dir, filename)

    if os.path.exists(output_path):
        print(f"  {filename} already exists, skipping.")
        return True

    # First try the end-to-end export from HuggingFace
    # If not available, fall back to exporting via ultralytics
    url = f"{HF_BASE_URL}/{filename}"

    print(f"  Downloading {filename} from {url}...")

    headers = {}
    if token:
        headers["Authorization"] = f"Bearer {token}"

    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req) as response:
            total = int(response.headers.get("Content-Length", 0))
            downloaded = 0
            block_size = 8192

            with open(output_path, "wb") as f:
                while True:
                    block = response.read(block_size)
                    if not block:
                        break
                    f.write(block)
                    downloaded += len(block)
                    if total > 0:
                        pct = downloaded * 100 // total
                        print(f"\r  {filename}: {pct}% ({downloaded // 1024}KB / {total // 1024}KB)", end="", flush=True)

            print(f"\r  {filename}: done ({downloaded // 1024}KB)")
            return True

    except urllib.error.HTTPError as e:
        print(f"\n  Failed to download {filename}: HTTP {e.code}")
        if e.code == 404:
            print(f"  Pre-built ONNX not found on HuggingFace. Trying local export...")
            return export_model_locally(variant, output_path)
        return False
    except urllib.error.URLError as e:
        print(f"\n  Failed to download {filename}: {e.reason}")
        return False


def export_model_locally(variant, output_path):
    """Export model using ultralytics Python package if available."""
    try:
        from ultralytics import YOLO
    except ImportError:
        print("  Install ultralytics to export models locally: pip install ultralytics")
        return False

    model_name = f"yolo26{variant}.pt"
    print(f"  Exporting {model_name} to ONNX (end-to-end, NMS-free)...")

    try:
        model = YOLO(model_name)
        model.export(format="onnx", imgsz=640)

        # ultralytics exports to same dir as .pt file
        exported = model_name.replace(".pt", ".onnx")
        if os.path.exists(exported):
            os.rename(exported, output_path)
            print(f"  Exported to {output_path}")
            return True
    except Exception as e:
        print(f"  Export failed: {e}")

    return False


def main():
    parser = argparse.ArgumentParser(description="Download YOLO26 ONNX models for Witness")
    parser.add_argument("--token", help="HuggingFace API token for authenticated downloads")
    parser.add_argument("--variants", nargs="+", choices=list(MODELS.keys()), default=list(MODELS.keys()),
                        help="Model variants to download (default: all)")
    parser.add_argument("--output", default=None, help="Output directory (default: models/ next to this script)")
    args = parser.parse_args()

    # Default output: models/ directory at repo root
    if args.output:
        output_dir = args.output
    else:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        output_dir = os.path.join(os.path.dirname(script_dir), "models")

    os.makedirs(output_dir, exist_ok=True)

    print(f"Downloading YOLO26 models to: {output_dir}")
    print()

    success = 0
    failed = 0
    for variant in args.variants:
        info = MODELS[variant]
        print(f"[{variant}] {info['description']}")
        if download_model(variant, output_dir, args.token):
            success += 1
        else:
            failed += 1
        print()

    print(f"Done: {success} downloaded, {failed} failed.")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
