"""Convert a tangent-space normal map to a heightmap via Frankot-Chellappa
FFT integration. Writes a 16-bit grayscale PNG for inspection.

Usage:
    python normal_to_height.py <input_normal.dds> [output.png]

Math:
    Tangent-space normal n = (nx, ny, nz), z up. Height gradient is:
        dh/du = -nx / nz
        dh/dv = -ny / nz
    Frankot-Chellappa recovers h from gradients in frequency domain:
        H(u,v) = (-i*u*Fp + -i*v*Fq) / (u^2 + v^2)
    where Fp, Fq are FFTs of dh/du, dh/dv. Sets DC term to 0 (mean height).
"""
import sys
import numpy as np
from pathlib import Path
from PIL import Image


def normal_to_height(normal_rgb: np.ndarray) -> np.ndarray:
    """normal_rgb: (H, W, 3) uint8 in [0, 255]. Returns (H, W) float in [0, 1]."""
    n = normal_rgb[..., :3].astype(np.float32) / 127.5 - 1.0
    nx, ny, nz = n[..., 0], n[..., 1], n[..., 2]
    nz = np.maximum(nz, 0.01)

    p = -nx / nz
    q = -ny / nz

    h, w = p.shape
    fy = np.fft.fftfreq(h).reshape(-1, 1) * h
    fx = np.fft.fftfreq(w).reshape(1, -1) * w

    Fp = np.fft.fft2(p)
    Fq = np.fft.fft2(q)

    denom = fx * fx + fy * fy
    denom[0, 0] = 1.0
    H = (-1j * fx * Fp + -1j * fy * Fq) / denom
    H[0, 0] = 0.0

    height = np.real(np.fft.ifft2(H))

    lo, hi = np.percentile(height, [0.5, 99.5])
    height = np.clip((height - lo) / max(hi - lo, 1e-9), 0.0, 1.0)
    return height


def process_one(in_path: Path, out_dir: Path, write_compare: bool):
    img = Image.open(in_path).convert("RGBA")
    arr = np.array(img)

    height = normal_to_height(arr)
    height_u16 = (height * 65535.0 + 0.5).astype(np.uint16)
    out_stem = in_path.stem.replace("_NML", "_HGT")
    Image.fromarray(height_u16, mode="I;16").save(out_dir / (out_stem + ".png"))

    # Raw binary: u32 width, u32 height, then width*height u16 pixels (row-major).
    # Runtime D3D11 reads this directly and creates an R16_UNORM Texture2D.
    bin_path = out_dir / (out_stem + ".bin")
    h, w = height_u16.shape
    with open(bin_path, "wb") as f:
        f.write(np.array([w, h], dtype=np.uint32).tobytes())
        f.write(height_u16.tobytes())

    if write_compare:
        dif_path = in_path.with_name(in_path.stem.replace("_NML", "_DIF") + in_path.suffix)
        if dif_path.exists():
            target_w = 512
            scale = target_w / arr.shape[1]
            target_h = int(arr.shape[0] * scale)
            dif = Image.open(dif_path).convert("RGB").resize((target_w, target_h), Image.LANCZOS)
            nml = Image.fromarray(arr[..., :3], mode="RGB").resize((target_w, target_h), Image.LANCZOS)
            hgt = Image.fromarray((height * 255).astype(np.uint8), mode="L").resize(
                (target_w, target_h), Image.LANCZOS).convert("RGB")
            canvas = Image.new("RGB", (target_w * 3, target_h), (0, 0, 0))
            canvas.paste(dif, (0, 0))
            canvas.paste(nml, (target_w, 0))
            canvas.paste(hgt, (target_w * 2, 0))
            canvas.save(out_dir / (in_path.stem.replace("_NML", "_HGT") + "_compare.png"))


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    in_arg = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else in_arg.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    if in_arg.is_dir():
        files = sorted(in_arg.glob("*_NML.dds"))
        print(f"Batch: {len(files)} normal maps in {in_arg}")
        for i, f in enumerate(files):
            process_one(f, out_dir, write_compare=True)
            print(f"  [{i+1}/{len(files)}] {f.name}")
    else:
        process_one(in_arg, out_dir, write_compare=True)
        print(f"Done: {in_arg.name}")


if __name__ == "__main__":
    main()
