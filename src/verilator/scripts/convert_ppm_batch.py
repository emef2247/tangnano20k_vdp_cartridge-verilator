#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
convert_ppm_batch.py
Batch-convert display_*.ppm images to PNG / BMP / JPG using Pillow.

Usage:
  ./convert_ppm_batch.py [options]

Examples:
  # convert all display_*.ppm -> display_*.png (in-place)
  ./convert_ppm_batch.py --format png

  # convert and place outputs in out_png/ directory
  ./convert_ppm_batch.py --format png --outdir out_png

  # convert only files matching a pattern
  ./convert_ppm_batch.py --pattern "display_000*.ppm" --format jpg --quality 90

  # force overwrite existing outputs
  ./convert_ppm_batch.py --format png --overwrite

Requirements:
  Python 3 and Pillow (PIL). Install Pillow with:
    pip install Pillow
"""
from __future__ import annotations
import argparse
import sys
from pathlib import Path
from PIL import Image, UnidentifiedImageError

def parse_args():
    p = argparse.ArgumentParser(description="Batch-convert PPM images to PNG/BMP/JPG")
    p.add_argument("--pattern", "-p", default="display_*.ppm",
                   help="Glob pattern to match input files (default: display_*.ppm)")
    p.add_argument("--format", "-f", choices=("png","bmp","jpg"), required=True,
                   help="Output image format")
    p.add_argument("--outdir", "-o", default="",
                   help="Output directory (default: same directory as input files)")
    p.add_argument("--quality", "-q", type=int, default=95,
                   help="JPEG quality (only applies to jpg, default: 95)")
    p.add_argument("--overwrite", action="store_true",
                   help="Overwrite existing output files")
    p.add_argument("--verbose", "-v", action="store_true",
                   help="Verbose output")
    return p.parse_args()

def convert_file(src: Path, dest: Path, out_format: str, quality: int, overwrite: bool, verbose: bool):
    if dest.exists() and not overwrite:
        if verbose:
            print(f"skip (exists): {dest}")
        return True

    try:
        with Image.open(src) as im:
            # Convert paletted or single-channel to RGB for formats like JPG
            if out_format == "jpg":
                if im.mode in ("RGBA","LA"):
                    # JPEG doesn't support alpha: composite over black
                    bg = Image.new("RGB", im.size, (0,0,0))
                    bg.paste(im, mask=im.split()[-1])
                    out_im = bg
                else:
                    out_im = im.convert("RGB")
            else:
                # For PNG/BMP keep as-is but use RGB for safety
                if im.mode not in ("RGB","RGBA"):
                    out_im = im.convert("RGB")
                else:
                    out_im = im.copy()

            # Ensure parent dir exists
            dest.parent.mkdir(parents=True, exist_ok=True)

            if out_format == "jpg":
                out_im.save(dest, format="JPEG", quality=quality, optimize=True)
            elif out_format == "png":
                out_im.save(dest, format="PNG", optimize=True)
            else: # bmp
                out_im.save(dest, format="BMP")
    except UnidentifiedImageError:
        print(f"error: cannot identify image file {src}", file=sys.stderr)
        return False
    except Exception as e:
        print(f"error: failed to convert {src} -> {dest}: {e}", file=sys.stderr)
        return False

    if verbose:
        print(f"converted: {src} -> {dest}")
    return True

def main():
    args = parse_args()

    try:
        import PIL
    except Exception:
        print("Pillow (PIL) is required. Install with: pip install Pillow", file=sys.stderr)
        return 2

    pattern = args.pattern
    files = sorted(Path(".").glob(pattern))
    if not files:
        print(f"No files found matching pattern: {pattern}", file=sys.stderr)
        return 1

    outdir = Path(args.outdir) if args.outdir else None
    out_format = args.format.lower()
    quality = max(1, min(100, args.quality))

    ok = True
    for src in files:
        src = src.resolve()
        stem = src.stem  # filename without suffix (e.g. display_000001)
        out_name = f"{stem}.{out_format}"
        if outdir:
            dest = (outdir / out_name).resolve()
        else:
            dest = (src.parent / out_name).resolve()
        if not convert_file(src, dest, out_format, quality, args.overwrite, args.verbose):
            ok = False

    return 0 if ok else 3

if __name__ == "__main__":
    sys.exit(main())

