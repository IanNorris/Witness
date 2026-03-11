#!/usr/bin/env python3
"""Download ONNX models for Witness detection and face recognition.

Downloads pretrained weights and exports/extracts to ONNX format.
Requires: pip install ultralytics (for YOLO), pip install insightface (for face recognition)
"""

import argparse
import os
import shutil
import sys
import zipfile

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
        model.export(format="onnx", imgsz=640, opset=21)

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


def download_face_recognition(output_dir):
    """Download MobileFaceNet (w600k_mbf) from InsightFace buffalo_sc pack."""
    onnx_path = os.path.join(output_dir, "face_recognition.onnx")
    if os.path.exists(onnx_path):
        print(f"  face_recognition.onnx already exists, skipping.")
        return True

    # Try using insightface package (handles caching)
    try:
        from insightface.utils import storage
        import urllib.request

        url = "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_sc.zip"
        cache_dir = os.path.join(os.path.expanduser("~"), ".insightface", "models")
        os.makedirs(cache_dir, exist_ok=True)
        zip_path = os.path.join(cache_dir, "buffalo_sc.zip")
        extract_dir = os.path.join(cache_dir, "buffalo_sc")
        mbf_path = os.path.join(extract_dir, "w600k_mbf.onnx")

        if not os.path.exists(mbf_path):
            if not os.path.exists(zip_path):
                print(f"  Downloading buffalo_sc.zip (~14MB...")
                urllib.request.urlretrieve(url, zip_path)
            print(f"  Extracting w600k_mbf.onnx...")
            with zipfile.ZipFile(zip_path, "r") as zf:
                zf.extractall(extract_dir)

        if os.path.exists(mbf_path):
            shutil.copy2(mbf_path, onnx_path)
            size_mb = os.path.getsize(onnx_path) / (1024 * 1024)
            print(f"  Saved: {onnx_path} ({size_mb:.1f} MB)")
            return True
        else:
            print(f"  w600k_mbf.onnx not found in extracted pack.")
            return False

    except Exception as e:
        # Fallback: direct download without insightface
        try:
            import urllib.request

            url = "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_sc.zip"
            zip_path = os.path.join(output_dir, "buffalo_sc.zip")
            print(f"  Downloading buffalo_sc.zip (~14MB)...")
            urllib.request.urlretrieve(url, zip_path)
            print(f"  Extracting w600k_mbf.onnx...")
            with zipfile.ZipFile(zip_path, "r") as zf:
                for name in zf.namelist():
                    if "w600k_mbf" in name:
                        data = zf.read(name)
                        with open(onnx_path, "wb") as f:
                            f.write(data)
                        break
            os.remove(zip_path)
            if os.path.exists(onnx_path):
                size_mb = os.path.getsize(onnx_path) / (1024 * 1024)
                print(f"  Saved: {onnx_path} ({size_mb:.1f} MB)")
                return True
            else:
                print(f"  w600k_mbf.onnx not found in zip.")
                return False
        except Exception as e2:
            print(f"  Download failed: {e2}")
            return False


def main():
    parser = argparse.ArgumentParser(description="Download ONNX models for Witness")
    parser.add_argument("--variants", nargs="+", choices=list(MODELS.keys()), default=["n"],
                        help="YOLO model variants to download (default: n)")
    parser.add_argument("--all", action="store_true", help="Download all YOLO model variants + face recognition")
    parser.add_argument("--face", action="store_true", help="Download face recognition model (MobileFaceNet)")
    parser.add_argument("--output", default=None, help="Output directory (default: models/ at repo root)")
    args = parser.parse_args()

    if args.all:
        args.variants = list(MODELS.keys())
        args.face = True

    # Default output: models/ directory at repo root
    if args.output:
        output_dir = args.output
    else:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        output_dir = os.path.join(os.path.dirname(script_dir), "models")

    os.makedirs(output_dir, exist_ok=True)

    print(f"Exporting YOLO26 models to: {output_dir}")
    print(f"Variants: {', '.join(args.variants)}")
    if args.face:
        print(f"Face recognition: MobileFaceNet (w600k_mbf)")
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

    if args.face:
        print("[face] MobileFaceNet w600k_mbf (~13MB, 512-dim, 99.7% LFW)")
        if download_face_recognition(output_dir):
            success += 1
        else:
            failed += 1
        print()

    print(f"Done: {success} exported, {failed} failed.")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
