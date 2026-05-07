"""View a baked-heightmap dump from TerrainTess (file: baked_first.bin).

Format:
    u32 width, u32 height, u32 slices, then `slices` planes of width*height
    half-floats (R16_FLOAT, little-endian).

Usage:
    python view_baked_heightmap.py <path/to/baked_first.bin> [out_dir]

Saves one PNG per slice + a combined 6-up grid.
"""
import sys
import numpy as np
from pathlib import Path
from PIL import Image


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    in_path = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else in_path.parent

    with open(in_path, "rb") as f:
        hdr = np.frombuffer(f.read(12), dtype=np.uint32)
        w, h, slices = int(hdr[0]), int(hdr[1]), int(hdr[2])
        # Auto-detect: file size minus header should be slices*w*h * (2 or 4 bytes).
        rest = f.read()
    expected_u16 = slices * w * h * 2
    if len(rest) == expected_u16:
        raw = np.frombuffer(rest, dtype=np.uint16)
        arr = raw.reshape(slices, h, w).astype(np.float32) / 65535.0
        kind = "uint16"
    else:
        raw = np.frombuffer(rest, dtype=np.float16)
        arr = raw.reshape(slices, h, w).astype(np.float32)
        kind = "float16"
    print(f"Read {w}x{h} x{slices} {kind} planes ({len(rest)} bytes)")
    for s in range(slices):
        plane = arr[s]
        lo, hi = float(plane.min()), float(plane.max())
        norm = np.clip((plane - lo) / max(hi - lo, 1e-9), 0, 1)
        img = (norm * 255).astype(np.uint8)
        out_path = out_dir / f"baked_slice_{s}.png"
        Image.fromarray(img, mode="L").save(out_path)
        print(f"  slice {s}: range=[{lo:.4f}, {hi:.4f}]  -> {out_path}")

    # Combined 3x2 grid scaled down for at-a-glance comparison.
    tile_w = 384
    tile_h = int(h * tile_w / w)
    grid = Image.new("L", (tile_w * 3, tile_h * 2), 0)
    for s in range(min(slices, 6)):
        plane = arr[s]
        lo, hi = float(plane.min()), float(plane.max())
        norm = np.clip((plane - lo) / max(hi - lo, 1e-9), 0, 1)
        img = Image.fromarray((norm * 255).astype(np.uint8), mode="L").resize(
            (tile_w, tile_h), Image.LANCZOS)
        col, row = s % 3, s // 3
        grid.paste(img, (col * tile_w, row * tile_h))
    grid_path = out_dir / "baked_grid.png"
    grid.save(grid_path)
    print(f"  grid -> {grid_path}")


if __name__ == "__main__":
    main()
