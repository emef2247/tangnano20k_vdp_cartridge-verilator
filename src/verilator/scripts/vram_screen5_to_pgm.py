#!/usr/bin/env python3
import sys
import pathlib

def vram_screen5_to_pgm(vram_bytes: bytes, out_path: pathlib.Path):
    """
    vram_bytes: 128KB (SCREEN5 VRAM そのものとして扱う)
    out_path: 出力 PGM ファイルパス
    簡易モデル: 画面 256x212, 4bpp packed (2 pixels / byte)
    """
    width  = 256
    height = 212

    if len(vram_bytes) < width * height // 2:
        raise ValueError(f"VRAM size too small: {len(vram_bytes)} bytes")

    # ひとまず VRAM 先頭から 256x212ぶんを 4bpp として読む
    base = 0  # 必要ならここに PN/PG オフセットを反映させる
    line_bytes = width // 2  # 2pixel per byte

    # PGM (binary, P5) を出力
    with out_path.open("wb") as f:
        # header
        f.write(f"P5\n{width} {height}\n255\n".encode("ascii"))

        for y in range(height):
            line = bytearray(width)
            for bx in range(line_bytes):
                b = vram_bytes[base + y * line_bytes + bx]
                left  = (b >> 4) & 0x0F  # x=2*bx
                right = b & 0x0F         # x=2*bx+1
                # 4bit → 8bit グレースケール (0..15 -> 0..255)
                line[2 * bx]     = left * 17
                line[2 * bx + 1] = right * 17
            f.write(line)

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} vram.bin out.pgm", file=sys.stderr)
        sys.exit(1)

    vram_path = pathlib.Path(sys.argv[1])
    out_path  = pathlib.Path(sys.argv[2])

    vram_bytes = vram_path.read_bytes()
    vram_screen5_to_pgm(vram_bytes, out_path)
    print(f"Wrote {out_path}")

if __name__ == "__main__":
    main()

