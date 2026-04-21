#!/usr/bin/env python3
"""
Dust Survey Analyzer — Phase 1 Pipeline Mapping Tool

Reads all survey captures and produces a comprehensive report covering:
- Per-capture and aggregate draw call statistics
- Render pass identification and classification
- Universal shader inventory (union across all captures)
- GBuffer pass analysis
- Shadow pass analysis
- Deferred lighting identification
- Post-processing chain mapping
- Instancing candidates
- Constant buffer decoding for key pipeline shaders
"""

import json
import os
import sys
import struct
import base64
import glob
from collections import defaultdict, Counter
from pathlib import Path

# ── Helpers ──────────────────────────────────────────────────────

def decode_cb_data(b64: str) -> bytes:
    """Decode base64-encoded constant buffer data."""
    return base64.b64decode(b64)

def read_floats(data: bytes, offset: int, count: int) -> list:
    """Read `count` 32-bit floats from `data` at byte `offset`."""
    end = offset + count * 4
    if end > len(data):
        return []
    return list(struct.unpack_from(f'<{count}f', data, offset))

def read_float4x4(data: bytes, offset: int) -> list:
    """Read a 4x4 float matrix (64 bytes) from `data` at byte `offset`."""
    return read_floats(data, offset, 16)

def mat_is_identity_ish(m: list, tol=0.01) -> bool:
    if len(m) != 16:
        return False
    ident = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
    return all(abs(a - b) < tol for a, b in zip(m, ident))

def mat_looks_like_projection(m: list) -> bool:
    """Heuristic: projection matrices have m[15]==0 and m[11]!=0."""
    if len(m) != 16:
        return False
    return abs(m[15]) < 0.01 and abs(m[11]) > 0.01

def rt_signature(draw: dict) -> str:
    """Compact string summarizing the render target configuration."""
    rts = draw.get('renderTargets', [])
    ds = draw.get('depthStencil')
    parts = [f"{r['format']} {r['width']}x{r['height']}" for r in rts]
    sig = ' + '.join(parts) if parts else 'NO_RT'
    if ds:
        sig += f" | DS={ds['format']} {ds['width']}x{ds['height']}"
    return sig

def classify_pass(rts: list, ds: dict, draw: dict) -> str:
    """Classify a render pass based on RT configuration and draw properties."""
    num_rts = len(rts)
    if num_rts == 0:
        return "UNKNOWN_NO_RT"

    fmt0 = rts[0]['format']
    w, h = rts[0]['width'], rts[0]['height']

    # GBuffer: 3 MRTs at native res with B8G8R8A8 + B8G8R8A8 + R32_FLOAT
    if num_rts == 3:
        fmts = [r['format'] for r in rts]
        if 'R32_FLOAT' in fmts and fmts.count('B8G8R8A8_UNORM') == 2:
            return "GBUFFER_FILL"

    if num_rts == 1:
        topo = draw.get('topology', '')
        hs = draw.get('hs')
        typ = draw.get('type', '')

        # Shadow map: large square R32_FLOAT with depth
        if fmt0 == 'R32_FLOAT' and ds and w == h and w >= 1024:
            return "SHADOW_MAP"

        # Cubemap/reflection probe: 512x512 B8G8R8A8 with depth
        if fmt0 == 'B8G8R8A8_UNORM' and ds and w == 512 and h == 512:
            return "CUBEMAP_RENDER"

        # Luminance chain: progressive R32_FLOAT downsampling
        if fmt0 == 'R32_FLOAT' and w <= 512 and h <= 512:
            if w == h or h <= 2:
                return "LUMINANCE_CHAIN"

        # Deferred lighting: R11G11B10_FLOAT at native res
        if fmt0 == 'R11G11B10_FLOAT' and w >= 1920:
            return "HDR_PASS"

        # Tonemapping / LDR: B8G8R8A8_UNORM at native res
        if fmt0 == 'B8G8R8A8_UNORM' and w >= 1920:
            return "LDR_PASS"

        # FXAA / final: R8G8B8A8_UNORM at native res
        if fmt0 == 'R8G8B8A8_UNORM' and w >= 1920:
            return "FINAL_OUTPUT"

        # Stencil mask: R8_UNORM at native res
        if fmt0 == 'R8_UNORM' and w >= 1920:
            return "STENCIL_MASK"

        # HDR temp: R16G16B16A16_FLOAT at native res
        if fmt0 == 'R16G16B16A16_FLOAT' and w >= 1920:
            return "HDR_TEMP"

        # Half-res: B8G8R8A8 at half native
        if fmt0 == 'B8G8R8A8_UNORM' and w >= 640 and w < 1920:
            return "HALF_RES_POST"

        # Bloom chain: R11G11B10_FLOAT at various sub-native resolutions
        if fmt0 == 'R11G11B10_FLOAT' and w < 1920:
            return "BLOOM_CHAIN"

        # DOF: R16_FLOAT at native
        if fmt0 == 'R16_FLOAT' and w >= 1920:
            return "DOF_PASS"

        # Small R32_FLOAT
        if fmt0 == 'R32_FLOAT':
            return "LUMINANCE_CHAIN"

    # UI: R8G8B8A8_UNORM with no depth, at native res
    if num_rts == 1 and fmt0 == 'R8G8B8A8_UNORM' and not ds:
        return "UI_OVERLAY"

    return "UNKNOWN"


# ── Data Loading ─────────────────────────────────────────────────

def load_capture(capture_dir: str) -> dict:
    """Load all frame JSONs and summary from a capture directory."""
    summary_path = os.path.join(capture_dir, 'survey_summary.json')
    summary = None
    if os.path.isfile(summary_path):
        with open(summary_path) as f:
            summary = json.load(f)

    frames = []
    for fpath in sorted(glob.glob(os.path.join(capture_dir, 'frame_*.json'))):
        with open(fpath) as f:
            frames.append(json.load(f))

    return {
        'name': os.path.basename(capture_dir),
        'dir': capture_dir,
        'summary': summary,
        'frames': frames,
    }


def find_shader_source(shader_dirs: list, addr: str) -> str | None:
    """Find HLSL source for a shader address across all capture shader dirs."""
    if not addr:
        return None
    clean = addr.replace('0x', '')
    for d in shader_dirs:
        for prefix in ('ps_', 'vs_', 'gs_', 'hs_', 'ds_'):
            path = os.path.join(d, f"{prefix}{addr}.hlsl")
            if os.path.isfile(path):
                with open(path, 'r', errors='replace') as f:
                    return f.read()
    return None


# ── Analysis Functions ───────────────────────────────────────────

def analyze_passes(frame: dict) -> list:
    """Identify render passes by RT configuration changes."""
    draws = frame.get('draws', [])
    passes = []
    current_pass = None
    prev_rt_sig = None

    for d in draws:
        sig = rt_signature(d)
        if sig != prev_rt_sig:
            if current_pass:
                passes.append(current_pass)
            rts = d.get('renderTargets', [])
            ds = d.get('depthStencil')
            current_pass = {
                'start_index': d['index'],
                'end_index': d['index'],
                'rt_signature': sig,
                'classification': classify_pass(rts, ds, d),
                'draw_count': 1,
                'draw_types': Counter({d['type']: 1}),
                'topologies': Counter({d.get('topology', '?'): 1}),
                'vs_set': {d.get('vs', '?')},
                'ps_set': {d.get('ps', '?')},
                'has_tessellation': d.get('hs') is not None,
                'rts': rts,
                'ds': ds,
            }
            prev_rt_sig = sig
        else:
            current_pass['end_index'] = d['index']
            current_pass['draw_count'] += 1
            current_pass['draw_types'][d['type']] += 1
            current_pass['topologies'][d.get('topology', '?')] += 1
            current_pass['vs_set'].add(d.get('vs', '?'))
            current_pass['ps_set'].add(d.get('ps', '?'))
            if d.get('hs'):
                current_pass['has_tessellation'] = True

    if current_pass:
        passes.append(current_pass)

    return passes


def find_deferred_lighting_draw(frame: dict) -> dict | None:
    """Find the first fullscreen draw to R11G11B10_FLOAT at native res after GBuffer."""
    draws = frame.get('draws', [])
    saw_gbuffer = False
    for d in draws:
        rts = d.get('renderTargets', [])
        if len(rts) == 3:
            saw_gbuffer = True
            continue
        if saw_gbuffer and len(rts) == 1 and rts[0]['format'] == 'R11G11B10_FLOAT' and rts[0]['width'] >= 1920:
            if d.get('type') in ('Draw', 'DrawIndexed') and d.get('topology') == 'TRIANGLELIST':
                ic = d.get('indexCount', d.get('vertexCount', 0))
                if ic <= 6:
                    return d
    return None


def find_gbuffer_draws(frame: dict) -> list:
    """Find all draws in the GBuffer pass (3 MRT configuration)."""
    draws = frame.get('draws', [])
    result = []
    for d in draws:
        rts = d.get('renderTargets', [])
        if len(rts) == 3:
            fmts = [r['format'] for r in rts]
            if 'R32_FLOAT' in fmts and fmts.count('B8G8R8A8_UNORM') == 2:
                result.append(d)
    return result


def find_shadow_draws(frame: dict) -> list:
    """Find all draws targeting a large R32_FLOAT with depth (shadow map)."""
    draws = frame.get('draws', [])
    result = []
    for d in draws:
        rts = d.get('renderTargets', [])
        ds = d.get('depthStencil')
        if len(rts) == 1 and rts[0]['format'] == 'R32_FLOAT' and ds:
            w, h = rts[0]['width'], rts[0]['height']
            if w == h and w >= 1024:
                result.append(d)
    return result


def find_cubemap_draws(frame: dict) -> list:
    """Find draws to 512x512 B8G8R8A8 with depth (cubemap/probe)."""
    draws = frame.get('draws', [])
    result = []
    for d in draws:
        rts = d.get('renderTargets', [])
        ds = d.get('depthStencil')
        if len(rts) == 1 and rts[0]['format'] == 'B8G8R8A8_UNORM' and ds:
            if rts[0]['width'] == 512 and rts[0]['height'] == 512:
                result.append(d)
    return result


def analyze_instancing(gbuffer_draws: list) -> list:
    """Find instancing candidates: repeated mesh signatures in GBuffer pass."""
    mesh_groups = defaultdict(list)
    for d in gbuffer_draws:
        vs = d.get('vs', '?')
        ps = d.get('ps', '?')
        stride = d.get('vertexStride', 0)
        idx_count = d.get('indexCount', 0)
        key = (vs, ps, stride, idx_count)
        mesh_groups[key].append(d)

    candidates = []
    for key, group in sorted(mesh_groups.items(), key=lambda x: -len(x[1])):
        if len(group) >= 2:
            candidates.append({
                'vs': key[0],
                'ps': key[1],
                'vertexStride': key[2],
                'indexCount': key[3],
                'instances': len(group),
                'potential_draw_reduction': len(group) - 1,
            })
    return candidates


def analyze_cb_layout(draws: list, max_draws=50) -> dict:
    """Analyze VS constant buffer data across multiple draws to find matrix offsets."""
    results = {
        'cb_sizes': Counter(),
        'slot_usage': Counter(),
        'matrix_candidates': [],
    }

    for d in draws[:max_draws]:
        for cb in d.get('vsCBs', []):
            slot = cb['slot']
            size = cb['size']
            results['cb_sizes'][(slot, size)] += 1
            results['slot_usage'][slot] += 1

            if 'data' in cb and size >= 64:
                raw = decode_cb_data(cb['data'])
                # Look for 4x4 matrices at 64-byte aligned offsets
                for off in range(0, min(len(raw), 512) - 63, 16):
                    m = read_float4x4(raw, off)
                    if not m:
                        continue
                    # Check if it looks like a matrix (has non-zero, non-trivial values)
                    nonzero = sum(1 for v in m if abs(v) > 0.001)
                    if 4 <= nonzero <= 16:
                        if mat_looks_like_projection(m):
                            results['matrix_candidates'].append({
                                'offset': off,
                                'type': 'projection_like',
                                'draw_index': d['index'],
                            })
                        elif not mat_is_identity_ish(m):
                            results['matrix_candidates'].append({
                                'offset': off,
                                'type': 'transform',
                                'draw_index': d['index'],
                            })

    return results


def build_shader_inventory(all_captures: list) -> dict:
    """Build a universal shader inventory across all captures."""
    ps_usage = defaultdict(lambda: {'captures': set(), 'passes': set(), 'draw_count': 0})
    vs_usage = defaultdict(lambda: {'captures': set(), 'passes': set(), 'draw_count': 0})

    for cap in all_captures:
        cap_name = cap['name']
        for frame in cap['frames']:
            passes = analyze_passes(frame)
            draw_to_pass = {}
            for p in passes:
                for idx in range(p['start_index'], p['end_index'] + 1):
                    draw_to_pass[idx] = p['classification']

            for d in frame.get('draws', []):
                ps = d.get('ps')
                vs = d.get('vs')
                pass_class = draw_to_pass.get(d['index'], 'UNKNOWN')
                if ps:
                    ps_usage[ps]['captures'].add(cap_name)
                    ps_usage[ps]['passes'].add(pass_class)
                    ps_usage[ps]['draw_count'] += 1
                if vs:
                    vs_usage[vs]['captures'].add(cap_name)
                    vs_usage[vs]['passes'].add(pass_class)
                    vs_usage[vs]['draw_count'] += 1

    return {'pixel_shaders': dict(ps_usage), 'vertex_shaders': dict(vs_usage)}


def analyze_deferred_lighting_cb(draw: dict) -> dict | None:
    """Decode and analyze the deferred lighting shader's constant buffers."""
    if not draw:
        return None

    result = {'ps_cbs': [], 'vs_cbs': []}

    for cb_list_key, out_key in [('psCBs', 'ps_cbs'), ('vsCBs', 'vs_cbs')]:
        for cb in draw.get(cb_list_key, []):
            info = {'slot': cb['slot'], 'size': cb['size']}
            if 'data' in cb:
                raw = decode_cb_data(cb['data'])
                floats = read_floats(raw, 0, min(len(raw) // 4, 128))
                info['float_values'] = floats
                info['raw_size'] = len(raw)

                # Look for recognizable patterns
                annotations = []
                for i in range(0, len(floats) - 3, 4):
                    vec4 = floats[i:i+4]
                    # Normalized direction vector
                    length = sum(v*v for v in vec4[:3]) ** 0.5
                    if 0.95 < length < 1.05 and all(abs(v) <= 1.01 for v in vec4[:3]):
                        annotations.append({'offset': i*4, 'register': f'c{i//4}', 'guess': 'direction_vector', 'values': vec4})
                    # Color-like (0-1 range, positive)
                    elif all(0.0 <= v <= 2.0 for v in vec4[:3]) and any(v > 0.01 for v in vec4[:3]):
                        if not any(abs(v) > 10 for v in vec4):
                            annotations.append({'offset': i*4, 'register': f'c{i//4}', 'guess': 'color_or_param', 'values': vec4})

                info['annotations'] = annotations
            result[out_key].append(info)

    return result


# ── Report Generation ────────────────────────────────────────────

def generate_report(all_captures: list, shader_dirs: list, output_path: str):
    """Generate the full analysis report."""
    lines = []
    def w(s=''):
        lines.append(s)

    w("=" * 80)
    w("DUST SURVEY ANALYSIS REPORT")
    w(f"Captures analyzed: {len(all_captures)}")
    w(f"Total frames: {sum(len(c['frames']) for c in all_captures)}")
    w("=" * 80)

    # ── 1. Per-Capture Overview ──
    w()
    w("=" * 80)
    w("1. PER-CAPTURE OVERVIEW")
    w("=" * 80)
    w()
    w(f"{'Capture':<50s} {'Draws':>6s} {'GBuf':>5s} {'Shadow':>7s} {'Cube':>5s} {'PS':>4s} {'VS':>4s}")
    w("-" * 80)

    all_gbuffer_counts = []
    all_shadow_counts = []
    all_draw_counts = []

    for cap in all_captures:
        if not cap['frames']:
            continue
        frame = cap['frames'][0]
        draws = frame.get('draws', [])
        gb = find_gbuffer_draws(frame)
        sh = find_shadow_draws(frame)
        cb = find_cubemap_draws(frame)
        summary = cap['summary'] or {}
        ps_count = summary.get('uniquePixelShaders', '?')
        vs_count = summary.get('uniqueVertexShaders', '?')

        all_draw_counts.append(len(draws))
        all_gbuffer_counts.append(len(gb))
        all_shadow_counts.append(len(sh))

        w(f"{cap['name']:<50s} {len(draws):>6d} {len(gb):>5d} {len(sh):>7d} {len(cb):>5d} {ps_count:>4} {vs_count:>4}")

    w()
    if all_draw_counts:
        w(f"Draw counts — min: {min(all_draw_counts)}, max: {max(all_draw_counts)}, avg: {sum(all_draw_counts)//len(all_draw_counts)}")
        w(f"GBuffer draws — min: {min(all_gbuffer_counts)}, max: {max(all_gbuffer_counts)}, avg: {sum(all_gbuffer_counts)//len(all_gbuffer_counts)}")
        w(f"Shadow draws — min: {min(all_shadow_counts)}, max: {max(all_shadow_counts)}, avg: {sum(all_shadow_counts)//len(all_shadow_counts)}")

    # ── 2. Pipeline Structure ──
    w()
    w("=" * 80)
    w("2. PIPELINE STRUCTURE (from densest capture)")
    w("=" * 80)
    w()

    densest = max(all_captures, key=lambda c: len(c['frames'][0]['draws']) if c['frames'] else 0)
    frame0 = densest['frames'][0]
    passes = analyze_passes(frame0)

    w(f"Source: {densest['name']}")
    w(f"Total draws in frame: {len(frame0['draws'])}")
    w(f"Render passes identified: {len(passes)}")
    w()
    w(f"{'Pass':<3s} {'Draws':>6s} {'Range':>15s} {'Classification':<20s} {'Tess':>4s} {'VS#':>4s} {'PS#':>4s} {'RT Signature'}")
    w("-" * 120)

    for i, p in enumerate(passes):
        tess = 'YES' if p['has_tessellation'] else ''
        rng = f"{p['start_index']}-{p['end_index']}"
        w(f"{i:<3d} {p['draw_count']:>6d} {rng:>15s} {p['classification']:<20s} {tess:>4s} {len(p['vs_set']):>4d} {len(p['ps_set']):>4d} {p['rt_signature']}")

    # ── 3. GBuffer Analysis ──
    w()
    w("=" * 80)
    w("3. GBUFFER PASS ANALYSIS")
    w("=" * 80)
    w()

    gb_draws = find_gbuffer_draws(frame0)
    if gb_draws:
        w(f"GBuffer draws: {len(gb_draws)}")
        w(f"RT format: {gb_draws[0]['renderTargets'][0]['format']} + {gb_draws[0]['renderTargets'][1]['format']} + {gb_draws[0]['renderTargets'][2]['format']}")
        w(f"Resolution: {gb_draws[0]['renderTargets'][0]['width']}x{gb_draws[0]['renderTargets'][0]['height']}")
        ds = gb_draws[0].get('depthStencil')
        if ds:
            w(f"Depth: {ds['format']} {ds['width']}x{ds['height']}")

        # Topology breakdown
        topo_counts = Counter(d.get('topology', '?') for d in gb_draws)
        w(f"\nTopology breakdown:")
        for t, c in topo_counts.most_common():
            w(f"  {t}: {c} draws")

        # Draw type breakdown
        type_counts = Counter(d.get('type', '?') for d in gb_draws)
        w(f"\nDraw type breakdown:")
        for t, c in type_counts.most_common():
            w(f"  {t}: {c} draws")

        # Shader variety
        gb_ps = set(d.get('ps') for d in gb_draws if d.get('ps'))
        gb_vs = set(d.get('vs') for d in gb_draws if d.get('vs'))
        w(f"\nUnique pixel shaders in GBuffer: {len(gb_ps)}")
        for ps in sorted(gb_ps):
            count = sum(1 for d in gb_draws if d.get('ps') == ps)
            w(f"  {ps}: {count} draws")
        w(f"\nUnique vertex shaders in GBuffer: {len(gb_vs)}")
        for vs in sorted(gb_vs):
            count = sum(1 for d in gb_draws if d.get('vs') == vs)
            w(f"  {vs}: {count} draws")

        # Tessellation in GBuffer?
        tess_draws = [d for d in gb_draws if d.get('hs')]
        w(f"\nTessellated GBuffer draws: {len(tess_draws)}")

        # SRV analysis
        srv_formats = Counter()
        srv_dims = Counter()
        for d in gb_draws:
            for srv in d.get('psSRVs', []):
                srv_formats[srv.get('format', '?')] += 1
                dim = f"{srv.get('width','?')}x{srv.get('height','?')}"
                srv_dims[dim] += 1
        w(f"\nTexture (SRV) formats bound during GBuffer:")
        for fmt, c in srv_formats.most_common():
            w(f"  {fmt}: {c} bindings")

        # Vertex stride analysis
        strides = Counter(d.get('vertexStride', 0) for d in gb_draws)
        w(f"\nVertex strides:")
        for s, c in strides.most_common():
            w(f"  {s} bytes: {c} draws")

        # Instancing candidates
        w(f"\n--- Instancing Candidates ---")
        candidates = analyze_instancing(gb_draws)
        total_reduction = sum(c['potential_draw_reduction'] for c in candidates)
        w(f"Total unique mesh signatures: {len(candidates)}")
        w(f"Total potential draw reduction: {total_reduction} / {len(gb_draws)} ({100*total_reduction/max(1,len(gb_draws)):.1f}%)")
        w(f"\nTop 20 most-repeated meshes:")
        for c in candidates[:20]:
            w(f"  VS={c['vs']} PS={c['ps']} stride={c['vertexStride']} idxCount={c['indexCount']}: {c['instances']}x (save {c['potential_draw_reduction']} draws)")

    # ── 4. Shadow Pass Analysis ──
    w()
    w("=" * 80)
    w("4. SHADOW PASS ANALYSIS")
    w("=" * 80)
    w()

    sh_draws = find_shadow_draws(frame0)
    if sh_draws:
        w(f"Shadow draws: {len(sh_draws)}")
        w(f"Shadow map resolution: {sh_draws[0]['renderTargets'][0]['width']}x{sh_draws[0]['renderTargets'][0]['height']}")
        w(f"Shadow map format: {sh_draws[0]['renderTargets'][0]['format']}")
        ds = sh_draws[0].get('depthStencil')
        if ds:
            w(f"Shadow depth: {ds['format']}")

        topo_counts = Counter(d.get('topology', '?') for d in sh_draws)
        w(f"\nTopology breakdown:")
        for t, c in topo_counts.most_common():
            w(f"  {t}: {c} draws")

        tess_count = sum(1 for d in sh_draws if d.get('hs'))
        w(f"Tessellated draws: {tess_count} / {len(sh_draws)}")

        sh_vs = set(d.get('vs') for d in sh_draws if d.get('vs'))
        sh_ps = set(d.get('ps') for d in sh_draws if d.get('ps'))
        sh_hs = set(d.get('hs') for d in sh_draws if d.get('hs'))
        sh_ds_set = set(d.get('ds') for d in sh_draws if d.get('ds'))
        w(f"\nUnique shaders — VS: {len(sh_vs)}, PS: {len(sh_ps)}, HS: {len(sh_hs)}, DS: {len(sh_ds_set)}")
        w(f"VS: {sorted(sh_vs)}")
        w(f"PS: {sorted(sh_ps)}")
        if sh_hs:
            w(f"HS: {sorted(sh_hs)}")
        if sh_ds_set:
            w(f"DS (domain): {sorted(sh_ds_set)}")

        type_counts = Counter(d.get('type', '?') for d in sh_draws)
        w(f"\nDraw types: {dict(type_counts)}")

        # Check if shadow pass shares geometry with GBuffer
        gb_vs_set = set(d.get('vs') for d in gb_draws if d.get('vs')) if gb_draws else set()
        shared_vs = sh_vs & gb_vs_set
        w(f"\nVertex shaders shared with GBuffer: {len(shared_vs)} / {len(sh_vs)}")

        # Shadow pass position in frame
        if sh_draws:
            first_sh = sh_draws[0]['index']
            last_sh = sh_draws[-1]['index']
            deferred = find_deferred_lighting_draw(frame0)
            deferred_idx = deferred['index'] if deferred else '?'
            gb_last = gb_draws[-1]['index'] if gb_draws else '?'
            w(f"\nShadow pass draw range: [{first_sh} - {last_sh}]")
            w(f"GBuffer last draw: {gb_last}")
            w(f"Deferred lighting draw: {deferred_idx}")
            if isinstance(deferred_idx, int) and first_sh > deferred_idx:
                w("*** SHADOW MAP RENDERED AFTER DEFERRED LIGHTING — likely consumed next frame or in a separate pass ***")

    # ── 5. Deferred Lighting Analysis ──
    w()
    w("=" * 80)
    w("5. DEFERRED LIGHTING SHADER")
    w("=" * 80)
    w()

    deferred = find_deferred_lighting_draw(frame0)
    if deferred:
        w(f"Draw index: {deferred['index']}")
        w(f"Type: {deferred['type']}")
        w(f"VS: {deferred.get('vs')}")
        w(f"PS: {deferred.get('ps')}")
        w(f"Topology: {deferred.get('topology')}")
        w(f"Index count: {deferred.get('indexCount', 'N/A')}")

        w(f"\nSRVs bound to PS:")
        for srv in deferred.get('psSRVs', []):
            w(f"  slot {srv['slot']}: {srv['format']} {srv.get('width','?')}x{srv.get('height','?')} ({srv.get('dimension','?')})")

        w(f"\nSRVs bound to VS:")
        for srv in deferred.get('vsSRVs', []):
            w(f"  slot {srv['slot']}: {srv['format']} {srv.get('width','?')}x{srv.get('height','?')}")

        # CB analysis
        cb_info = analyze_deferred_lighting_cb(deferred)
        if cb_info:
            for stage, key in [('PS', 'ps_cbs'), ('VS', 'vs_cbs')]:
                for cb in cb_info[key]:
                    w(f"\n{stage} CB slot {cb['slot']} ({cb['size']} bytes):")
                    if 'float_values' in cb:
                        floats = cb['float_values']
                        for i in range(0, len(floats), 4):
                            reg = f"c{i//4}"
                            vals = floats[i:i+4]
                            if len(vals) == 4:
                                w(f"  {reg}: ({vals[0]:12.6f}, {vals[1]:12.6f}, {vals[2]:12.6f}, {vals[3]:12.6f})")
                    if 'annotations' in cb:
                        w(f"\n  Annotations:")
                        for ann in cb['annotations']:
                            w(f"    {ann['register']} @ byte {ann['offset']}: {ann['guess']} = {[f'{v:.4f}' for v in ann['values']]}")

        # Check consistency across captures
        w(f"\n--- Deferred lighting PS consistency across captures ---")
        deferred_ps_set = set()
        deferred_vs_set = set()
        for cap in all_captures:
            for frame in cap['frames']:
                dl = find_deferred_lighting_draw(frame)
                if dl:
                    deferred_ps_set.add(dl.get('ps'))
                    deferred_vs_set.add(dl.get('vs'))
        w(f"Unique deferred PS across all captures: {deferred_ps_set}")
        w(f"Unique deferred VS across all captures: {deferred_vs_set}")
        if len(deferred_ps_set) == 1:
            w("CONFIRMED: Single deferred lighting pixel shader across all scenes.")
        else:
            w("WARNING: Multiple deferred lighting pixel shaders detected — may have permutations.")

    # ── 6. Cubemap/Reflection Pass ──
    w()
    w("=" * 80)
    w("6. CUBEMAP / REFLECTION PROBE PASS")
    w("=" * 80)
    w()

    cube_draws = find_cubemap_draws(frame0)
    if cube_draws:
        w(f"Cubemap draws: {len(cube_draws)}")
        w(f"Resolution: 512x512")
        cube_ps = set(d.get('ps') for d in cube_draws if d.get('ps'))
        cube_vs = set(d.get('vs') for d in cube_draws if d.get('vs'))
        w(f"Unique PS: {len(cube_ps)}, VS: {len(cube_vs)}")

        gb_ps_set = set(d.get('ps') for d in gb_draws if d.get('ps'))
        shared_ps = cube_ps & gb_ps_set
        w(f"PS shared with GBuffer: {len(shared_ps)} / {len(cube_ps)}")

        cube_topo = Counter(d.get('topology', '?') for d in cube_draws)
        w(f"Topology: {dict(cube_topo)}")
    else:
        w("No cubemap pass detected.")

    # ── 7. Post-Processing Chain ──
    w()
    w("=" * 80)
    w("7. POST-PROCESSING CHAIN")
    w("=" * 80)
    w()

    post_passes = [p for p in passes if p['classification'] in (
        'HDR_PASS', 'STENCIL_MASK', 'HDR_TEMP', 'LDR_PASS',
        'LUMINANCE_CHAIN', 'BLOOM_CHAIN', 'HALF_RES_POST',
        'DOF_PASS', 'FINAL_OUTPUT')]

    for p in post_passes:
        shaders = f"PS={sorted(p['ps_set'])} VS={sorted(p['vs_set'])}"
        w(f"  [{p['start_index']:>4d}-{p['end_index']:>4d}] {p['classification']:<20s} draws={p['draw_count']:>3d}  {p['rt_signature']}")

    # ── 8. Universal Shader Inventory ──
    w()
    w("=" * 80)
    w("8. SHADER INVENTORY (across all captures)")
    w("=" * 80)
    w()

    inventory = build_shader_inventory(all_captures)

    w(f"Total unique pixel shaders: {len(inventory['pixel_shaders'])}")
    w(f"Total unique vertex shaders: {len(inventory['vertex_shaders'])}")

    w(f"\n--- Pixel Shaders by Pass Classification ---")
    ps_by_pass = defaultdict(set)
    for addr, info in inventory['pixel_shaders'].items():
        for p in info['passes']:
            ps_by_pass[p].add(addr)
    for pass_name in sorted(ps_by_pass.keys()):
        addrs = ps_by_pass[pass_name]
        w(f"\n  {pass_name} ({len(addrs)} PS):")
        for a in sorted(addrs):
            info = inventory['pixel_shaders'][a]
            w(f"    {a}: {info['draw_count']} draws across {len(info['captures'])} captures")

    w(f"\n--- Vertex Shaders by Pass Classification ---")
    vs_by_pass = defaultdict(set)
    for addr, info in inventory['vertex_shaders'].items():
        for p in info['passes']:
            vs_by_pass[p].add(addr)
    for pass_name in sorted(vs_by_pass.keys()):
        addrs = vs_by_pass[pass_name]
        w(f"\n  {pass_name} ({len(addrs)} VS):")
        for a in sorted(addrs):
            info = inventory['vertex_shaders'][a]
            w(f"    {a}: {info['draw_count']} draws across {len(info['captures'])} captures")

    # ── 9. VS Constant Buffer Layout (GBuffer draws) ──
    w()
    w("=" * 80)
    w("9. VS CONSTANT BUFFER LAYOUT (GBuffer draws)")
    w("=" * 80)
    w()

    if gb_draws:
        cb_analysis = analyze_cb_layout(gb_draws, max_draws=30)
        w(f"CB size distribution (slot, size) -> count:")
        for (slot, size), count in cb_analysis['cb_sizes'].most_common():
            w(f"  slot {slot}, {size} bytes: {count} draws")

        w(f"\nSlot usage: {dict(cb_analysis['slot_usage'])}")

        # Sample a few draws to show actual CB content
        w(f"\n--- Sample VS CB data (first 3 GBuffer draws) ---")
        for d in gb_draws[:3]:
            w(f"\nDraw {d['index']} (VS={d.get('vs')}, idxCount={d.get('indexCount')}):")
            for cb in d.get('vsCBs', []):
                if 'data' not in cb:
                    continue
                raw = decode_cb_data(cb['data'])
                floats = read_floats(raw, 0, min(len(raw)//4, 64))
                w(f"  CB slot {cb['slot']} ({cb['size']} bytes, {len(raw)} decoded):")
                for i in range(0, len(floats), 4):
                    vals = floats[i:i+4]
                    if len(vals) == 4:
                        w(f"    c{i//4}: ({vals[0]:12.6f}, {vals[1]:12.6f}, {vals[2]:12.6f}, {vals[3]:12.6f})")

    # ── 10. Cross-Capture Consistency ──
    w()
    w("=" * 80)
    w("10. CROSS-CAPTURE CONSISTENCY")
    w("=" * 80)
    w()

    # Check if pipeline structure is consistent
    pass_sequences = []
    for cap in all_captures:
        if cap['frames']:
            passes_c = analyze_passes(cap['frames'][0])
            seq = [p['classification'] for p in passes_c]
            pass_sequences.append((cap['name'], seq))

    # Find common subsequence of classifications
    all_class_sets = [set(s) for _, s in pass_sequences]
    common_classes = set.intersection(*all_class_sets) if all_class_sets else set()
    all_classes = set.union(*all_class_sets) if all_class_sets else set()

    w(f"Pass types present in ALL captures: {sorted(common_classes)}")
    w(f"Pass types present in SOME captures: {sorted(all_classes - common_classes)}")

    # Shadow map resolution consistency
    shadow_resolutions = set()
    for cap in all_captures:
        if cap['frames']:
            sh = find_shadow_draws(cap['frames'][0])
            if sh:
                res = f"{sh[0]['renderTargets'][0]['width']}x{sh[0]['renderTargets'][0]['height']}"
                shadow_resolutions.add(res)
    w(f"\nShadow map resolutions across captures: {shadow_resolutions}")

    # Write report
    report = '\n'.join(lines)
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(report)
    print(report.encode('ascii', errors='replace').decode('ascii'))
    print(f"\nReport written to: {output_path}")


# ── Main ─────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: survey_analyze.py <survey_root_dir> [output_report_path]")
        print("  survey_root_dir: directory containing timestamped capture subdirectories")
        sys.exit(1)

    survey_root = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(survey_root, 'analysis_report.txt')

    if not os.path.isdir(survey_root):
        print(f"Error: {survey_root} is not a directory")
        sys.exit(1)

    print(f"Loading captures from: {survey_root}")
    captures = []
    shader_dirs = []

    for entry in sorted(os.listdir(survey_root)):
        cap_dir = os.path.join(survey_root, entry)
        if not os.path.isdir(cap_dir):
            continue
        if not any(f.startswith('frame_') for f in os.listdir(cap_dir)):
            continue
        print(f"  Loading: {entry}...")
        cap = load_capture(cap_dir)
        captures.append(cap)
        shader_dir = os.path.join(cap_dir, 'shaders')
        if os.path.isdir(shader_dir):
            shader_dirs.append(shader_dir)

    if not captures:
        print("No captures found.")
        sys.exit(1)

    print(f"\nLoaded {len(captures)} captures, generating report...")
    generate_report(captures, shader_dirs, output_path)


if __name__ == '__main__':
    main()
