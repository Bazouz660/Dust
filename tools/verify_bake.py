"""Verify the runtime C++ FFT bake against the Python reference implementation.

Inputs:
    baked_source_rgba.bin  (decoded normal-map slices the runtime saw)
    baked_first_u16.bin    (the runtime's heightmap output)

For each of 6 slices:
    1. Identify which Kenshi *_NML.dds source the slice content matches (by hashing).
    2. Run the trusted Python Frankot-Chellappa on the captured RGBA → reference height.
    3. Diff reference vs runtime height. Print pixel-wise stats.
    4. Save a side-by-side comparison PNG: source | runtime | reference.

Usage:
    python verify_bake.py <workshop_or_mod_dir> [out_dir]
"""
import sys
import hashlib
import numpy as np
from pathlib import Path
from PIL import Image

# Reuse the FFT integration logic from normal_to_height.py.
sys.path.insert(0, str(Path(__file__).parent))
from normal_to_height import normal_to_height


KENSHI_NML_DIR = Path(r"D:\SteamLibrary\steamapps\common\Kenshi\data\newland\land\textures")


def fingerprint_rgba(rgba: np.ndarray, size: int = 64) -> np.ndarray:
    """Downsample to size×size RGB and return as float32 fingerprint. Robust
    to BC3 quantization differences between GPU linear-sampled decode and PIL
    raw decode (they only differ in fine quantization noise)."""
    img = Image.fromarray(rgba[..., :3], mode="RGB").resize(
        (size, size), Image.LANCZOS)
    return np.array(img).astype(np.float32) / 255.0


def index_kenshi_textures():
    print(f"Indexing {KENSHI_NML_DIR}...")
    index = []  # list of (name, fingerprint)
    for f in sorted(KENSHI_NML_DIR.glob("*_NML.dds")):
        try:
            img = Image.open(f).convert("RGBA")
            arr = np.array(img)
            if arr.shape[0] >= 64 and arr.shape[1] >= 64:
                index.append((f.name, fingerprint_rgba(arr)))
        except Exception as e:
            print(f"  skip {f.name}: {e}")
    print(f"  indexed {len(index)} textures")
    return index


def best_match(rgba: np.ndarray, index):
    fp = fingerprint_rgba(rgba)
    best_name, best_dist = "<unknown>", float("inf")
    for name, ref in index:
        d = float(np.linalg.norm(fp - ref))
        if d < best_dist:
            best_dist, best_name = d, name
    return best_name, best_dist


def normalize_slice(arr: np.ndarray) -> np.ndarray:
    lo, hi = float(arr.min()), float(arr.max())
    rng = max(hi - lo, 1e-9)
    return (arr - lo) / rng


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    in_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).parent.parent / "tmp" / "verify"
    out_dir.mkdir(parents=True, exist_ok=True)

    src_path = in_dir / "baked_source_rgba.bin"
    h_path = in_dir / "baked_first_u16.bin"
    diffuse_path = in_dir / "baked_diffuse_rgba.bin"

    def load_rgba(path):
        with open(path, "rb") as f:
            hdr = np.frombuffer(f.read(12), dtype=np.uint32)
            return int(hdr[0]), int(hdr[1]), int(hdr[2]), np.frombuffer(f.read(), dtype=np.uint8)

    W, H, S, rgba_raw = load_rgba(src_path)
    sources = rgba_raw.reshape(S, H, W, 4)

    diffuses = None
    if diffuse_path.exists():
        Wd, Hd, Sd, d_raw = load_rgba(diffuse_path)
        if (Wd, Hd, Sd) == (W, H, S):
            diffuses = d_raw.reshape(S, H, W, 4)
            print(f"Loaded diffuse from {diffuse_path}")

    with open(h_path, "rb") as f:
        hdr = np.frombuffer(f.read(12), dtype=np.uint32)
        Wh, Hh, Sh = int(hdr[0]), int(hdr[1]), int(hdr[2])
        u16 = np.frombuffer(f.read(), dtype=np.uint16)
    runtimes = u16.reshape(Sh, Hh, Wh).astype(np.float32) / 65535.0

    assert (W, H, S) == (Wh, Hh, Sh), f"size mismatch: {(W,H,S)} vs {(Wh,Hh,Sh)}"
    print(f"Loaded {S} slices @ {W}x{H}")

    index = index_kenshi_textures()

    role_names = ["base", "slope", "cliff", "grass", "dirt", "road"]

    for s in range(S):
        src = sources[s]
        runtime = runtimes[s]
        match, dist = best_match(src, index)
        print(f"\nslice {s} ({role_names[s]}): matches={match}  L2-dist={dist:.2f}")

        reference = normal_to_height(src)
        # normal_to_height already normalizes to [0,1].

        # Diff stats.
        diff = runtime - reference
        print(f"  runtime range [{runtime.min():.4f}, {runtime.max():.4f}]")
        print(f"  reference range [{reference.min():.4f}, {reference.max():.4f}]")
        print(f"  abs-mean-diff: {np.abs(diff).mean():.4f}  max-diff: {np.abs(diff).max():.4f}")
        # Correlation coefficient — robust to scale shifts.
        rt_c = runtime - runtime.mean()
        rf_c = reference - reference.mean()
        denom = np.sqrt((rt_c * rt_c).sum() * (rf_c * rf_c).sum())
        corr = (rt_c * rf_c).sum() / max(denom, 1e-9)
        print(f"  correlation: {corr:.4f}  (1.0 = identical shape, 0 = uncorrelated)")

        # Side-by-side image: DIFFUSE | NML | runtime height | reference height.
        target_w = 384
        target_h = int(H * target_w / W)
        panels = []
        labels = []
        if diffuses is not None:
            dif = diffuses[s]
            dif_img = Image.fromarray(dif[..., :3], mode="RGB").resize((target_w, target_h), Image.LANCZOS)
            panels.append(dif_img); labels.append("DIFFUSE")
        nml_img = Image.fromarray(src[..., :3], mode="RGB").resize((target_w, target_h), Image.LANCZOS)
        panels.append(nml_img); labels.append("NORMAL")
        rt_img = Image.fromarray((normalize_slice(runtime) * 255).astype(np.uint8), mode="L").resize(
            (target_w, target_h), Image.LANCZOS).convert("RGB")
        panels.append(rt_img); labels.append("RUNTIME H")
        rf_img = Image.fromarray((normalize_slice(reference) * 255).astype(np.uint8), mode="L").resize(
            (target_w, target_h), Image.LANCZOS).convert("RGB")
        panels.append(rf_img); labels.append("REFERENCE H")
        canvas = Image.new("RGB", (target_w * len(panels), target_h), 0)
        for i, p in enumerate(panels):
            canvas.paste(p, (target_w * i, 0))
        out_path = out_dir / f"slice_{s}_{role_names[s]}.png"
        canvas.save(out_path)
        print(f"  -> {out_path} ({' | '.join(labels)})")


if __name__ == "__main__":
    main()
