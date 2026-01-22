from PIL import Image
import numpy as np

def vram_screen5_to_bmp(vram_bytes: bytes, out_path: pathlib.Path):
    width, height = 256, 212
    base = 0
    line_bytes = width // 2

    img = np.zeros((height, width), dtype=np.uint8)

    for y in range(height):
        for bx in range(line_bytes):
            b = vram_bytes[base + y * line_bytes + bx]
            left  = (b >> 4) & 0x0F
            right = b & 0x0F
            img[y, 2*bx]   = left
            img[y, 2*bx+1] = right

    # 4bit インデックス → 16色パレット（簡易:グレイスケール or 好きなRGB）
    palette = []
    for i in range(16):
        g = i * 17
        palette.extend([g, g, g])
    palette.extend([0,0,0] * (256-16))  # 残りは 0 埋め

    im = Image.fromarray(img, mode="P")
    im.putpalette(palette)
    im.save(out_path)  # 拡張子 .bmp / .png で自動判別

