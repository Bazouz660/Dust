#!/usr/bin/env python3
"""
Dust Survey Deep Analyzer — Exhaustive Pipeline Mapping

Maps every single draw call to its shader source, material type, and pipeline purpose.
Produces a definitive reference for implementing any visual feature in Kenshi.
"""

import json
import os
import sys
import struct
import base64
import glob
import re
from collections import defaultdict, Counter
from pathlib import Path


# ── Shader Source Map ────────────────────────────────────────────

MATERIAL_CATEGORIES = {
    'objects.hlsl':       'GEOMETRY_OBJECT',
    'terrainfp4.hlsl':    'GEOMETRY_TERRAIN',
    'terrain.hlsl':       'GEOMETRY_TERRAIN',
    'triplanar.hlsl':     'GEOMETRY_TRIPLANAR',
    'skin.hlsl':          'GEOMETRY_SKIN',
    'foliage.hlsl':       'GEOMETRY_FOLIAGE',
    'character.hlsl':     'GEOMETRY_CHARACTER',
    'construction.hlsl':  'GEOMETRY_CONSTRUCTION',
    'creature.hlsl':      'GEOMETRY_CREATURE',
    'distant_town.hlsl':  'GEOMETRY_DISTANT_TOWN',
    'basic.hlsl':         'GEOMETRY_BASIC',
    'deferred.hlsl':      'LIGHTING_DEFERRED',
    'fog.hlsl':           'ATMOSPHERE_FOG',
    'water.hlsl':         'FORWARD_WATER',
    'SkyX_Skydome.hlsl':  'SKY_DOME',
    'SkyX_Clouds.hlsl':   'SKY_CLOUDS',
    'SkyX_Ground.hlsl':   'SKY_GROUND',
    'SkyX_Moon.hlsl':     'SKY_MOON',
    'SkyX_Lightning.hlsl':'SKY_LIGHTNING',
    'SkyX_VolClouds.hlsl':'SKY_VOLCLOUDS',
    'SkyX_VolClouds_Lightning.hlsl': 'SKY_VOLCLOUDS',
    'moon.hlsl':          'SKY_MOON',
    'birds.hlsl':         'SKY_BIRDS',
    'shadowcaster.hlsl':  'SHADOW_CASTER',
    'rtwtessellator.hlsl':'SHADOW_TESSELLATOR',
    'rtwshadows.hlsl':    'SHADOW_RTW',
    'rtwforward.hlsl':    'RTW_FORWARD',
    'rtwbackward.hlsl':   'RTW_BACKWARD',
    'rtwdebug.hlsl':      'RTW_DEBUG',
    'hdrfp4.hlsl':        'POST_HDR',
    'quad_vp.hlsl':       'POST_QUAD_VP',
    'FXAA.hlsl':          'POST_FXAA',
    'FXAA2.hlsl':         'POST_FXAA',
    'heathaze.hlsl':      'POST_HEATHAZE',
    'DepthBlur.hlsl':     'POST_DOF',
    'CAS.hlsl':           'POST_SHARPEN',
    'Colourfulness.hlsl': 'POST_COLOR',
    'ColourfulPoster.hlsl':'POST_COLOR',
    'Comic.hlsl':         'POST_STYLIZE',
    'Kuwahara.hlsl':      'POST_STYLIZE',
    'AO.hlsl':            'POST_AO',
    'SSAOD.hlsl':         'POST_SSAO',
    'PassThrough.hlsl':   'POST_PASSTHROUGH',
    'rtticons.hlsl':      'UI_ICONS',
    'rtt.hlsl':           'UI_RTT',
    'basic2D.hlsl':       'UI_2D',
    'interiormask.hlsl':  'MASK_INTERIOR',
    'mapfeature.hlsl':    'MAP_FEATURE',
    'particles.hlsl':     'FORWARD_PARTICLES',
    'blood.hlsl':         'FORWARD_BLOOD',
}

KNOWN_PRECOMPILED = {
    '0x63DB45B8': {'type':'ps', 'source_name':'deferred.hlsl', 'entry_point':'main_fs', 'category':'LIGHTING_DEFERRED', 'detail_category':'DEFERRED_SUN'},
    '0x63862038': {'type':'ps', 'source_name':'deferred.hlsl', 'entry_point':'main_fs', 'category':'LIGHTING_DEFERRED', 'detail_category':'DEFERRED_SUN'},
    '0x63DB5878': {'type':'ps', 'source_name':'deferred.hlsl', 'entry_point':'light_fs', 'category':'LIGHTING_DEFERRED', 'detail_category':'DEFERRED_LIGHT_VOLUME'},
    '0x6394F578': {'type':'vs', 'source_name':'quad_vp.hlsl', 'entry_point':'quad_vs', 'category':'POST_QUAD_VP', 'detail_category':'POST_QUAD_VP'},
    '0x63862378': {'type':'vs', 'source_name':'quad_vp.hlsl', 'entry_point':'quad_vs', 'category':'POST_QUAD_VP', 'detail_category':'POST_QUAD_VP'},
    '0x66B35DB8': {'type':'hs', 'source_name':'rtwtessellator.hlsl', 'entry_point':'tessellator_hs', 'category':'SHADOW_TESSELLATOR', 'detail_category':'SHADOW_TESSELLATOR'},
    '0x66B366F8': {'type':'ds', 'source_name':'rtwtessellator.hlsl', 'entry_point':'tessellator_ds', 'category':'SHADOW_TESSELLATOR', 'detail_category':'SHADOW_TESSELLATOR'},
    '0x617F6738': {'type':'ps', 'source_name':'FXAA.hlsl', 'entry_point':'fxaa_ps', 'category':'POST_FXAA', 'detail_category':'POST_FXAA'},
    '0x617F6438': {'type':'vs', 'source_name':'FXAA.hlsl', 'entry_point':'fxaa_vs', 'category':'POST_FXAA', 'detail_category':'POST_FXAA'},
    '0x63DB6038': {'type':'ps', 'source_name':'heathaze.hlsl', 'entry_point':'heathaze_ps', 'category':'POST_HEATHAZE', 'detail_category':'POST_HEATHAZE'},
    '0x63DB5EB8': {'type':'vs', 'source_name':'heathaze.hlsl', 'entry_point':'heathaze_vs', 'category':'POST_HEATHAZE', 'detail_category':'POST_HEATHAZE'},
    '0x6394FA38': {'type':'ps', 'source_name':'stencil_mask.hlsl', 'entry_point':'stencil_ps', 'category':'PASS_STENCIL', 'detail_category':'STENCIL_WRITE'},
    '0x6394FBB8': {'type':'ps', 'source_name':'stencil_mask.hlsl', 'entry_point':'stencil_ps', 'category':'PASS_STENCIL', 'detail_category':'STENCIL_WRITE'},
    '0x63950378': {'type':'ps', 'source_name':'stencil_mask.hlsl', 'entry_point':'stencil_ps', 'category':'PASS_STENCIL', 'detail_category':'STENCIL_WRITE'},
    '0x6394F6F8': {'type':'vs', 'source_name':'stencil_mask.hlsl', 'entry_point':'stencil_vs', 'category':'PASS_STENCIL', 'detail_category':'STENCIL_WRITE'},
    '0x63DB3C78': {'type':'ps', 'source_name':'gbuffer_repack.hlsl', 'entry_point':'repack_ps', 'category':'PASS_REPACK', 'detail_category':'GBUFFER_REPACK'},
    '0x63DB40F8': {'type':'ps', 'source_name':'gbuffer_repack.hlsl', 'entry_point':'repack_ps', 'category':'PASS_REPACK', 'detail_category':'GBUFFER_REPACK'},
    '0x63DB42B8': {'type':'ps', 'source_name':'gbuffer_repack.hlsl', 'entry_point':'repack_ps', 'category':'PASS_REPACK', 'detail_category':'GBUFFER_REPACK'},
    '0x63DB3F78': {'type':'vs', 'source_name':'gbuffer_repack.hlsl', 'entry_point':'repack_vs', 'category':'PASS_REPACK', 'detail_category':'GBUFFER_REPACK'},
    '0x63DB50B8': {'type':'ps', 'source_name':'hdrfp4.hlsl', 'entry_point':'luminance_ps', 'category':'POST_HDR', 'detail_category':'LUMINANCE_ADAPT'},
    '0x63DB3478': {'type':'ps', 'source_name':'hdrfp4.hlsl', 'entry_point':'bloom_ps', 'category':'POST_HDR', 'detail_category':'BLOOM_BLUR_V'},
    '0x63DB59F8': {'type':'ps', 'source_name':'hdrfp4.hlsl', 'entry_point':'bloom_ps', 'category':'POST_HDR', 'detail_category':'BLOOM_BLUR_H'},
    '0x63DB5578': {'type':'ps', 'source_name':'hdrfp4.hlsl', 'entry_point':'bloom_ps', 'category':'POST_HDR', 'detail_category':'BLOOM_DOWNSAMPLE'},
    '0x63DB6CB8': {'type':'ps', 'source_name':'DepthBlur.hlsl', 'entry_point':'dof_ps', 'category':'POST_DOF', 'detail_category':'POST_DOF'},
    '0x63DB6378': {'type':'ps', 'source_name':'hdrfp4.hlsl', 'entry_point':'halfres_ps', 'category':'POST_HDR', 'detail_category':'POST_HDR'},
    '0x63DB5238': {'type':'ps', 'source_name':'hdrfp4.hlsl', 'entry_point':'halfres_ps', 'category':'POST_HDR', 'detail_category':'POST_HDR'},
    '0x63DB2FF8': {'type':'ps', 'source_name':'hdrfp4.hlsl', 'entry_point':'halfres_ps', 'category':'POST_HDR', 'detail_category':'POST_HDR'},
    '0x63DB4A78': {'type':'ps', 'source_name':'post_ldr.hlsl', 'entry_point':'post_ps', 'category':'POST_LDR', 'detail_category':'POST_LDR'},
    '0x63DB4BF8': {'type':'ps', 'source_name':'post_ldr.hlsl', 'entry_point':'post_ps', 'category':'POST_LDR', 'detail_category':'POST_LDR'},
    '0x63DB64F8': {'type':'ps', 'source_name':'post_ldr.hlsl', 'entry_point':'post_ps', 'category':'POST_LDR', 'detail_category':'POST_LDR'},
    '0x63DB4D78': {'type':'ps', 'source_name':'post_ldr.hlsl', 'entry_point':'post_ps', 'category':'POST_LDR', 'detail_category':'POST_LDR'},
    '0x63DB32F8': {'type':'ps', 'source_name':'post_ldr.hlsl', 'entry_point':'post_ps', 'category':'POST_LDR', 'detail_category':'POST_LDR'},
    '0x63DB3AB8': {'type':'ps', 'source_name':'post_ldr.hlsl', 'entry_point':'post_ps', 'category':'POST_LDR', 'detail_category':'POST_LDR'},
    '0x63DB6678': {'type':'vs', 'source_name':'post_ldr.hlsl', 'entry_point':'post_vs', 'category':'POST_LDR', 'detail_category':'POST_LDR'},
    '0x64447B38': {'type':'ps', 'source_name':'basic2D.hlsl', 'entry_point':'basic2d_ps', 'category':'UI_2D', 'detail_category':'UI_2D'},
    '0x64446238': {'type':'vs', 'source_name':'basic2D.hlsl', 'entry_point':'basic2d_vs', 'category':'UI_2D', 'detail_category':'UI_2D'},
    '0x644474F8': {'type':'ps', 'source_name':'rtticons.hlsl', 'entry_point':'rtticons_ps', 'category':'UI_ICONS', 'detail_category':'UI_ICONS'},
    '0x63E71578': {'type':'vs', 'source_name':'quad_vp.hlsl', 'entry_point':'quad_vs', 'category':'POST_QUAD_VP', 'detail_category':'POST_QUAD_VP'},
}

HDR_ENTRY_POINTS = {
    'Downsample2x2Luminance_fp4': 'LUMINANCE_DOWNSAMPLE',
    'Downsample3x3_fp4':          'BLOOM_DOWNSAMPLE',
    'Downsample3x3Brightpass_fp4':'BLOOM_BRIGHTPASS',
    'BloomBlurH_fp4':             'BLOOM_BLUR_H',
    'BloomBlurV_fp4':             'BLOOM_BLUR_V',
    'AdaptLuminance_fp4':         'LUMINANCE_ADAPT',
    'Copy_fp4':                   'HDR_COPY',
    'Composite_fp4':              'HDR_COMPOSITE',
}

DEFERRED_ENTRY_POINTS = {
    'main_fs':  'DEFERRED_SUN',
    'main_vs':  'DEFERRED_SUN_VS',
    'light_fs': 'DEFERRED_LIGHT_VOLUME',
    'light_vs': 'DEFERRED_LIGHT_VOLUME_VS',
}

FOG_ENTRY_POINTS = {
    'atmosphere_fog_fs': 'FOG_ATMOSPHERE',
    'atmosphere_fog_vs': 'FOG_ATMOSPHERE_VS',
    'ground_fog_fs':     'FOG_GROUND',
    'fog_planes_fs':     'FOG_VOLUME_PLANE',
    'fog_planes_vs':     'FOG_VOLUME_PLANE_VS',
    'fog_sphere_fs':     'FOG_VOLUME_SPHERE',
    'fog_beam_fs':       'FOG_VOLUME_BEAM',
}


def build_shader_source_map(shader_dirs):
    """Read all .hlsl files from all capture shader dirs, extract source info."""
    shader_map = {}
    for sdir in shader_dirs:
        if not os.path.isdir(sdir):
            continue
        for fname in os.listdir(sdir):
            if not fname.endswith('.hlsl'):
                continue
            # Extract address and shader type from filename
            m = re.match(r'(ps|vs|hs|ds|gs)_(0x[0-9A-Fa-f]+)\.hlsl', fname)
            if not m:
                continue
            shader_type = m.group(1)
            addr = m.group(2)

            if addr in shader_map:
                continue

            fpath = os.path.join(sdir, fname)
            try:
                with open(fpath, 'r', errors='replace') as f:
                    header_lines = []
                    for line in f:
                        header_lines.append(line.rstrip())
                        if len(header_lines) >= 15:
                            break

                info = {
                    'type': shader_type,
                    'address': addr,
                    'source_name': None,
                    'entry_point': None,
                    'target': None,
                    'category': None,
                    'detail_category': None,
                }

                for line in header_lines:
                    if '// Source name:' in line:
                        info['source_name'] = line.split(':', 1)[1].strip()
                    elif '// Entry point:' in line:
                        info['entry_point'] = line.split(':', 1)[1].strip()
                    elif '// Target:' in line:
                        info['target'] = line.split(':', 1)[1].strip()

                if info['source_name']:
                    info['category'] = MATERIAL_CATEGORIES.get(info['source_name'], 'UNKNOWN')

                    ep = info['entry_point'] or ''
                    if info['source_name'] == 'hdrfp4.hlsl':
                        info['detail_category'] = HDR_ENTRY_POINTS.get(ep, 'POST_HDR_UNKNOWN')
                    elif info['source_name'] == 'deferred.hlsl':
                        info['detail_category'] = DEFERRED_ENTRY_POINTS.get(ep, 'LIGHTING_DEFERRED_UNKNOWN')
                    elif info['source_name'] == 'fog.hlsl':
                        info['detail_category'] = FOG_ENTRY_POINTS.get(ep, 'FOG_UNKNOWN')
                    else:
                        info['detail_category'] = info['category']

                shader_map[addr] = info

            except Exception:
                pass

    for addr, info in KNOWN_PRECOMPILED.items():
        if addr not in shader_map:
            shader_map[addr] = dict(info, address=addr)

    return shader_map


# ── Helpers ──────────────────────────────────────────────────────

def decode_cb_data(b64):
    return base64.b64decode(b64)

def read_floats(data, offset, count):
    end = offset + count * 4
    if end > len(data):
        return []
    return list(struct.unpack_from(f'<{count}f', data, offset))

def rt_signature(draw):
    rts = draw.get('renderTargets', [])
    ds = draw.get('depthStencil')
    parts = [f"{r['format']} {r['width']}x{r['height']}" for r in rts]
    sig = ' + '.join(parts) if parts else 'NO_RT'
    if ds:
        sig += f" | DS={ds['format']} {ds['width']}x{ds['height']}"
    return sig


def classify_rt(rts, ds):
    """Classify based purely on render target config."""
    num_rts = len(rts)
    if num_rts == 0:
        return "NO_RT"
    fmt0 = rts[0]['format']
    w, h = rts[0]['width'], rts[0]['height']
    if num_rts == 3:
        fmts = [r['format'] for r in rts]
        if 'R32_FLOAT' in fmts and fmts.count('B8G8R8A8_UNORM') == 2:
            return "GBUFFER_MRT"
    if num_rts > 1:
        return "MULTI_RT"
    if fmt0 == 'R32_FLOAT' and ds and w == h and w >= 1024:
        return "SHADOW_RT"
    if fmt0 == 'B8G8R8A8_UNORM' and ds and w == 512 and h == 512:
        return "CUBEMAP_RT"
    if fmt0 == 'R11G11B10_FLOAT' and w >= 1920:
        return "HDR_NATIVE"
    if fmt0 == 'R11G11B10_FLOAT' and w < 1920:
        return "HDR_DOWNSCALE"
    if fmt0 == 'B8G8R8A8_UNORM' and w >= 1920:
        return "LDR_NATIVE"
    if fmt0 == 'B8G8R8A8_UNORM' and w >= 640 and w < 1920:
        return "LDR_HALFRES"
    if fmt0 == 'R8G8B8A8_UNORM' and w >= 1920:
        return "FINAL_RT"
    if fmt0 == 'R8_UNORM' and w >= 1920:
        return "MASK_RT"
    if fmt0 == 'R16G16B16A16_FLOAT' and w >= 1920:
        return "HDR_TEMP_RT"
    if fmt0 == 'R16_FLOAT' and w >= 1920:
        return "FLOAT16_NATIVE"
    if fmt0 == 'R32_FLOAT' and w <= 512:
        return "LUMINANCE_RT"
    return "OTHER_RT"


def deep_classify_draw(draw, shader_map):
    """Classify a single draw call using shader source + RT info."""
    rts = draw.get('renderTargets', [])
    ds = draw.get('depthStencil')
    rt_class = classify_rt(rts, ds)

    ps_addr = draw.get('ps')
    vs_addr = draw.get('vs')
    ps_info = shader_map.get(ps_addr, {})
    vs_info = shader_map.get(vs_addr, {})

    ps_source = ps_info.get('source_name', '')
    vs_source = vs_info.get('source_name', '')
    ps_entry = ps_info.get('entry_point', '')
    vs_entry = vs_info.get('entry_point', '')
    ps_detail = ps_info.get('detail_category', '')
    vs_detail = vs_info.get('detail_category', '')

    result = {
        'rt_class': rt_class,
        'ps_source': ps_source,
        'vs_source': vs_source,
        'ps_entry': ps_entry,
        'vs_entry': vs_entry,
        'ps_category': ps_info.get('category', 'UNKNOWN'),
        'vs_category': vs_info.get('category', 'UNKNOWN'),
        'ps_detail': ps_detail,
        'vs_detail': vs_detail,
    }

    # Deep classification logic
    if rt_class == 'GBUFFER_MRT':
        result['pass'] = 'GBUFFER'
        result['material'] = ps_info.get('category', 'UNKNOWN_MATERIAL')
        return result

    if rt_class == 'SHADOW_RT':
        result['pass'] = 'SHADOW'
        if draw.get('hs'):
            result['material'] = 'SHADOW_TESSELLATED'
        elif ps_source == 'shadowcaster.hlsl':
            result['material'] = 'SHADOW_STANDARD'
        else:
            result['material'] = 'SHADOW_OTHER'
        return result

    if rt_class == 'CUBEMAP_RT':
        result['pass'] = 'CUBEMAP'
        result['material'] = ps_info.get('category', 'UNKNOWN_MATERIAL')
        return result

    is_fullscreen = False
    topo = draw.get('topology', '')
    ic = draw.get('indexCount', draw.get('vertexCount', 0))
    if (topo == 'TRIANGLELIST' and ic <= 6) or (topo == 'TRIANGLESTRIP' and ic <= 4):
        is_fullscreen = True

    if rt_class == 'HDR_NATIVE':
        if ps_source == 'deferred.hlsl':
            if is_fullscreen:
                ep = ps_entry or ''
                if 'light' in ep.lower():
                    result['pass'] = 'LIGHT_VOLUME'
                    result['material'] = 'DEFERRED_LIGHT_VOLUME'
                else:
                    result['pass'] = 'DEFERRED_SUN'
                    result['material'] = 'DEFERRED_SUN'
            else:
                result['pass'] = 'LIGHT_VOLUME'
                result['material'] = 'DEFERRED_LIGHT_VOLUME'
        elif ps_source == 'fog.hlsl':
            result['pass'] = 'FOG'
            result['material'] = ps_detail or 'FOG'
        elif ps_source == 'water.hlsl':
            result['pass'] = 'WATER'
            result['material'] = 'FORWARD_WATER'
        elif ps_source and 'SkyX' in ps_source:
            result['pass'] = 'SKY'
            result['material'] = ps_info.get('category', 'SKY')
        elif ps_source == 'hdrfp4.hlsl':
            result['pass'] = 'POST_HDR'
            result['material'] = ps_detail or 'POST_HDR'
        elif ps_source == 'objects.hlsl' and vs_source == 'water.hlsl':
            result['pass'] = 'WATER_DECOR'
            result['material'] = 'WATER_SURFACE_DECOR'
        elif ps_source == 'basic.hlsl':
            if is_fullscreen:
                result['pass'] = 'FULLSCREEN_HDR'
            else:
                result['pass'] = 'FORWARD_BASIC'
            result['material'] = 'BASIC'
        elif ps_source == 'particles.hlsl':
            result['pass'] = 'FORWARD_PARTICLES'
            result['material'] = 'PARTICLES'
        elif ps_source == 'blood.hlsl':
            result['pass'] = 'FORWARD_BLOOD'
            result['material'] = 'BLOOD'
        else:
            result['pass'] = 'HDR_UNKNOWN'
            result['material'] = ps_source or 'UNKNOWN'
        return result

    if rt_class == 'HDR_DOWNSCALE':
        if ps_source == 'hdrfp4.hlsl':
            result['pass'] = 'BLOOM'
            result['material'] = ps_detail or 'BLOOM'
        else:
            result['pass'] = 'HDR_DOWNSCALE'
            result['material'] = ps_source or 'UNKNOWN'
        return result

    if rt_class == 'LDR_NATIVE':
        if ps_source == 'hdrfp4.hlsl':
            if 'Composite' in (ps_entry or ''):
                result['pass'] = 'TONEMAP_COMPOSITE'
            else:
                result['pass'] = 'POST_HDR'
            result['material'] = ps_detail or 'POST_HDR'
        elif ps_source in ('FXAA.hlsl', 'FXAA2.hlsl'):
            result['pass'] = 'FXAA'
            result['material'] = 'POST_FXAA'
        elif ps_source == 'DepthBlur.hlsl':
            result['pass'] = 'DOF'
            result['material'] = 'POST_DOF'
        elif ps_source == 'CAS.hlsl':
            result['pass'] = 'SHARPEN'
            result['material'] = 'POST_SHARPEN'
        elif ps_source == 'Colourfulness.hlsl':
            result['pass'] = 'COLOR_GRADE'
            result['material'] = 'POST_COLOR'
        elif ps_source == 'ColourfulPoster.hlsl':
            result['pass'] = 'COLOR_GRADE'
            result['material'] = 'POST_POSTER'
        elif ps_source == 'interiormask.hlsl':
            result['pass'] = 'INTERIOR_MASK'
            result['material'] = 'MASK_INTERIOR'
        elif ps_source == 'PassThrough.hlsl':
            result['pass'] = 'PASSTHROUGH'
            result['material'] = 'POST_PASSTHROUGH'
        else:
            result['pass'] = 'LDR_POST'
            result['material'] = ps_source or 'UNKNOWN'
        return result

    if rt_class == 'FINAL_RT':
        if ps_source in ('FXAA.hlsl', 'FXAA2.hlsl'):
            result['pass'] = 'FXAA'
            result['material'] = 'POST_FXAA'
        elif ps_source == 'heathaze.hlsl':
            result['pass'] = 'HEAT_HAZE'
            result['material'] = 'POST_HEATHAZE'
        elif ps_source == 'basic2D.hlsl':
            result['pass'] = 'UI'
            result['material'] = 'UI_2D'
        elif ps_source == 'rtticons.hlsl':
            result['pass'] = 'UI'
            result['material'] = 'UI_ICONS'
        else:
            result['pass'] = 'FINAL'
            result['material'] = ps_source or 'UNKNOWN'
        return result

    if rt_class == 'MASK_RT':
        result['pass'] = 'STENCIL_MASK'
        result['material'] = ps_source or 'MASK'
        return result

    if rt_class == 'HDR_TEMP_RT':
        result['pass'] = 'GBUFFER_REPACK'
        result['material'] = ps_source or 'REPACK'
        return result

    if rt_class == 'FLOAT16_NATIVE':
        result['pass'] = 'DOF_COC'
        result['material'] = ps_source or 'DOF'
        return result

    if rt_class == 'LDR_HALFRES':
        result['pass'] = 'HALF_RES_POST'
        result['material'] = ps_source or 'HALF_RES'
        return result

    if rt_class == 'LUMINANCE_RT':
        result['pass'] = 'LUMINANCE'
        result['material'] = ps_detail if ps_source == 'hdrfp4.hlsl' else (ps_source or 'LUMINANCE')
        return result

    result['pass'] = 'UNKNOWN'
    result['material'] = ps_source or 'UNKNOWN'
    return result


# ── Data Loading ─────────────────────────────────────────────────

def load_capture(capture_dir):
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


# ── Analysis Functions ───────────────────────────────────────────

def build_draw_timeline(frame, shader_map):
    """Classify every draw in the frame, group into semantic passes."""
    draws = frame.get('draws', [])
    timeline = []
    for d in draws:
        classification = deep_classify_draw(d, shader_map)
        timeline.append({
            'index': d['index'],
            'type': d.get('type', '?'),
            'topology': d.get('topology', '?'),
            'vs': d.get('vs'),
            'ps': d.get('ps'),
            'hs': d.get('hs'),
            'ds_shader': d.get('ds'),
            'indexCount': d.get('indexCount', d.get('vertexCount', 0)),
            'vertexStride': d.get('vertexStride', 0),
            'instanceCount': d.get('instanceCount', 1),
            'rt_sig': rt_signature(d),
            'classification': classification,
            'psSRVs': d.get('psSRVs', []),
            'vsSRVs': d.get('vsSRVs', []),
            'psCBs': d.get('psCBs', []),
            'vsCBs': d.get('vsCBs', []),
        })
    return timeline


def group_into_passes(timeline):
    """Group timeline draws into semantic passes."""
    passes = []
    current = None
    for entry in timeline:
        pass_name = entry['classification']['pass']
        if current is None or pass_name != current['pass_name']:
            if current:
                passes.append(current)
            current = {
                'pass_name': pass_name,
                'start_index': entry['index'],
                'end_index': entry['index'],
                'draws': [entry],
                'materials': Counter(),
                'ps_set': set(),
                'vs_set': set(),
                'ps_sources': set(),
                'vs_sources': set(),
            }
        else:
            current['end_index'] = entry['index']
            current['draws'].append(entry)

        mat = entry['classification'].get('material', 'UNKNOWN')
        current['materials'][mat] += 1
        if entry['ps']:
            current['ps_set'].add(entry['ps'])
        if entry['vs']:
            current['vs_set'].add(entry['vs'])
        current['ps_sources'].add(entry['classification'].get('ps_source', ''))
        current['vs_sources'].add(entry['classification'].get('vs_source', ''))

    if current:
        passes.append(current)
    return passes


def analyze_state_changes(timeline):
    """Track shader, texture, and CB changes between consecutive draws."""
    stats = {
        'total_draws': len(timeline),
        'ps_changes': 0,
        'vs_changes': 0,
        'rt_changes': 0,
        'srv_changes': 0,
        'redundant_ps': 0,
        'redundant_vs': 0,
    }
    for i in range(1, len(timeline)):
        prev, curr = timeline[i-1], timeline[i]
        if prev['ps'] != curr['ps']:
            stats['ps_changes'] += 1
        else:
            stats['redundant_ps'] += 1
        if prev['vs'] != curr['vs']:
            stats['vs_changes'] += 1
        else:
            stats['redundant_vs'] += 1
        if prev['rt_sig'] != curr['rt_sig']:
            stats['rt_changes'] += 1

        prev_srvs = frozenset((s['slot'], s.get('format'), s.get('width'), s.get('height'))
                              for s in prev.get('psSRVs', []))
        curr_srvs = frozenset((s['slot'], s.get('format'), s.get('width'), s.get('height'))
                              for s in curr.get('psSRVs', []))
        if prev_srvs != curr_srvs:
            stats['srv_changes'] += 1

    return stats


def build_resource_inventory(timeline):
    """Track all unique SRV resources and render targets."""
    srv_resources = {}
    rt_resources = {}

    for entry in timeline:
        pass_name = entry['classification']['pass']
        for srv_list_key in ('psSRVs', 'vsSRVs'):
            stage = 'PS' if 'ps' in srv_list_key else 'VS'
            for srv in entry.get(srv_list_key, []):
                key = (srv.get('format'), srv.get('width'), srv.get('height'),
                       srv.get('dimension', 'TEXTURE2D'))
                if key not in srv_resources:
                    srv_resources[key] = {
                        'format': srv.get('format'),
                        'width': srv.get('width'),
                        'height': srv.get('height'),
                        'dimension': srv.get('dimension', 'TEXTURE2D'),
                        'consumers': set(),
                        'slots': set(),
                        'bind_count': 0,
                    }
                srv_resources[key]['consumers'].add(pass_name)
                srv_resources[key]['slots'].add((stage, srv['slot']))
                srv_resources[key]['bind_count'] += 1

    return srv_resources


def build_full_shader_database(timeline, shader_map):
    """Map every shader address to its usage across the frame."""
    db = {}
    for entry in timeline:
        for shader_type_key in ('ps', 'vs', 'hs', 'ds_shader'):
            addr = entry.get(shader_type_key)
            if not addr:
                continue
            if addr not in db:
                info = shader_map.get(addr, {})
                db[addr] = {
                    'address': addr,
                    'type': info.get('type', shader_type_key[:2]),
                    'source_name': info.get('source_name', 'UNKNOWN'),
                    'entry_point': info.get('entry_point', 'UNKNOWN'),
                    'target': info.get('target', ''),
                    'category': info.get('category', 'UNKNOWN'),
                    'detail_category': info.get('detail_category', 'UNKNOWN'),
                    'passes': set(),
                    'draw_count': 0,
                    'draw_indices': [],
                }
            db[addr]['passes'].add(entry['classification']['pass'])
            db[addr]['draw_count'] += 1
            db[addr]['draw_indices'].append(entry['index'])
    return db


def analyze_cb_patterns(timeline, shader_map, target_pass, max_draws=10):
    """Decode CB data for draws in a specific pass."""
    results = []
    count = 0
    for entry in timeline:
        if entry['classification']['pass'] != target_pass:
            continue
        if count >= max_draws:
            break
        cb_data = {}
        for stage, key in [('VS', 'vsCBs'), ('PS', 'psCBs')]:
            for cb in entry.get(key, []):
                if 'data' not in cb:
                    continue
                raw = decode_cb_data(cb['data'])
                floats = read_floats(raw, 0, min(len(raw)//4, 64))
                cb_data[f"{stage}_slot{cb['slot']}"] = {
                    'size': cb['size'],
                    'floats': floats,
                }
        if cb_data:
            results.append({
                'draw_index': entry['index'],
                'vs': entry['vs'],
                'ps': entry['ps'],
                'ps_source': entry['classification'].get('ps_source', ''),
                'vs_source': entry['classification'].get('vs_source', ''),
                'cbs': cb_data,
            })
            count += 1
    return results


# ── Report Generation ────────────────────────────────────────────

def generate_report(all_captures, shader_map, output_path):
    lines = []
    def w(s=''):
        lines.append(s)

    # Use densest capture as reference
    densest = max(all_captures, key=lambda c: len(c['frames'][0]['draws']) if c['frames'] else 0)
    frame0 = densest['frames'][0]
    timeline = build_draw_timeline(frame0, shader_map)
    passes = group_into_passes(timeline)

    w("=" * 100)
    w("KENSHI RENDERING PIPELINE — EXHAUSTIVE ANALYSIS")
    w(f"Reference capture: {densest['name']} ({len(frame0['draws'])} draws)")
    w(f"Captures analyzed: {len(all_captures)}")
    w(f"Shader source files mapped: {len(set(v.get('source_name','') for v in shader_map.values() if v.get('source_name')))}")
    w(f"Total unique shaders in map: {len(shader_map)}")
    w("=" * 100)

    # ═══════════════════════════════════════════════════════════════
    # SECTION 1: COMPLETE PIPELINE TIMELINE
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("1. PIPELINE PASS SEQUENCE (semantic, not just RT-based)")
    w("=" * 100)
    w()
    w(f"{'#':<3} {'Pass':<25} {'Draws':>5} {'Range':<15} {'Materials':<50} {'PS Sources'}")
    w("-" * 150)

    for i, p in enumerate(passes):
        rng = f"{p['start_index']}-{p['end_index']}"
        mats = ', '.join(f"{m}:{c}" for m, c in p['materials'].most_common(5))
        srcs = ', '.join(s for s in sorted(p['ps_sources']) if s)
        w(f"{i:<3} {p['pass_name']:<25} {len(p['draws']):>5} {rng:<15} {mats:<50} {srcs}")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 2: DRAW-BY-DRAW TIMELINE (every single draw)
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("2. COMPLETE DRAW-BY-DRAW TIMELINE")
    w("=" * 100)
    w()
    w(f"{'Idx':>5} {'Type':<22} {'Topo':<18} {'Pass':<25} {'Material':<30} {'PS Source':<25} {'VS Source':<25} {'IdxCnt':>7} {'Inst':>4}")
    w("-" * 170)

    for entry in timeline:
        c = entry['classification']
        w(f"{entry['index']:>5} "
          f"{entry['type']:<22} "
          f"{entry['topology']:<18} "
          f"{c['pass']:<25} "
          f"{c.get('material',''):<30} "
          f"{c.get('ps_source',''):<25} "
          f"{c.get('vs_source',''):<25} "
          f"{entry['indexCount']:>7} "
          f"{entry.get('instanceCount', 1):>4}")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 3: GBUFFER MATERIAL BREAKDOWN
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("3. GBUFFER MATERIAL BREAKDOWN")
    w("=" * 100)
    w()

    gbuffer_draws = [e for e in timeline if e['classification']['pass'] == 'GBUFFER']
    mat_counts = Counter(e['classification'].get('material', 'UNKNOWN') for e in gbuffer_draws)

    w(f"Total GBuffer draws: {len(gbuffer_draws)}")
    w()
    w(f"{'Material Category':<35} {'Draws':>6} {'%':>6} {'Unique PS':>9} {'Unique VS':>9} {'PS Sources'}")
    w("-" * 120)

    for mat, count in mat_counts.most_common():
        mat_draws = [e for e in gbuffer_draws if e['classification'].get('material') == mat]
        ps_set = set(e['ps'] for e in mat_draws if e['ps'])
        vs_set = set(e['vs'] for e in mat_draws if e['vs'])
        sources = set(e['classification'].get('ps_source', '') for e in mat_draws)
        pct = 100 * count / max(1, len(gbuffer_draws))
        w(f"{mat:<35} {count:>6} {pct:>5.1f}% {len(ps_set):>9} {len(vs_set):>9} {', '.join(s for s in sorted(sources) if s)}")

    # Per-material shader details
    w()
    w("--- Per-Material Shader Details ---")
    for mat, _ in mat_counts.most_common():
        mat_draws = [e for e in gbuffer_draws if e['classification'].get('material') == mat]
        ps_counts = Counter(e['ps'] for e in mat_draws if e['ps'])
        vs_counts = Counter(e['vs'] for e in mat_draws if e['vs'])
        w(f"\n  {mat}:")
        w(f"    Pixel Shaders ({len(ps_counts)}):")
        for addr, cnt in ps_counts.most_common():
            info = shader_map.get(addr, {})
            ep = info.get('entry_point', '?')
            w(f"      {addr}: {cnt:>4} draws  entry={ep}")
        w(f"    Vertex Shaders ({len(vs_counts)}):")
        for addr, cnt in vs_counts.most_common():
            info = shader_map.get(addr, {})
            ep = info.get('entry_point', '?')
            w(f"      {addr}: {cnt:>4} draws  entry={ep}")

        # Vertex strides
        strides = Counter(e['vertexStride'] for e in mat_draws)
        w(f"    Vertex Strides: {dict(strides.most_common())}")

        # Topology
        topos = Counter(e['topology'] for e in mat_draws)
        w(f"    Topologies: {dict(topos.most_common())}")

        # Texture formats
        srv_fmts = Counter()
        for e in mat_draws:
            for srv in e.get('psSRVs', []):
                srv_fmts[srv.get('format', '?')] += 1
        if srv_fmts:
            w(f"    Texture Formats: {dict(srv_fmts.most_common())}")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 4: SHADOW SYSTEM DEEP ANALYSIS
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("4. SHADOW SYSTEM DEEP ANALYSIS")
    w("=" * 100)
    w()

    shadow_draws = [e for e in timeline if e['classification']['pass'] == 'SHADOW']
    w(f"Total shadow draws: {len(shadow_draws)}")

    shadow_mat_counts = Counter(e['classification'].get('material', '?') for e in shadow_draws)
    w(f"\nShadow draw breakdown:")
    for mat, count in shadow_mat_counts.most_common():
        w(f"  {mat}: {count} draws")

    shadow_vs = Counter(e['vs'] for e in shadow_draws if e['vs'])
    shadow_ps = Counter(e['ps'] for e in shadow_draws if e['ps'])
    shadow_hs = set(e['hs'] for e in shadow_draws if e['hs'])
    shadow_ds = set(e['ds_shader'] for e in shadow_draws if e['ds_shader'])

    w(f"\nShadow VS ({len(shadow_vs)} unique):")
    for addr, cnt in shadow_vs.most_common():
        info = shader_map.get(addr, {})
        w(f"  {addr}: {cnt:>4} draws  src={info.get('source_name','?')}  entry={info.get('entry_point','?')}")

    w(f"\nShadow PS ({len(shadow_ps)} unique):")
    for addr, cnt in shadow_ps.most_common():
        info = shader_map.get(addr, {})
        w(f"  {addr}: {cnt:>4} draws  src={info.get('source_name','?')}  entry={info.get('entry_point','?')}")

    if shadow_hs:
        w(f"\nShadow HS: {sorted(shadow_hs)}")
    if shadow_ds:
        w(f"Shadow DS: {sorted(shadow_ds)}")

    tess_count = sum(1 for e in shadow_draws if e['hs'])
    w(f"\nTessellated shadow draws: {tess_count} / {len(shadow_draws)} ({100*tess_count/max(1,len(shadow_draws)):.1f}%)")

    topo_counts = Counter(e['topology'] for e in shadow_draws)
    w(f"Topologies: {dict(topo_counts.most_common())}")

    # Shadow CB analysis
    shadow_cbs = analyze_cb_patterns(timeline, shader_map, 'SHADOW', max_draws=3)
    if shadow_cbs:
        w(f"\n--- Shadow VS Constant Buffers (first 3 draws) ---")
        for cb_info in shadow_cbs:
            w(f"\n  Draw {cb_info['draw_index']} (VS={cb_info['vs']}, src={cb_info['vs_source']}):")
            for cb_key, cb_data in cb_info['cbs'].items():
                w(f"    {cb_key} ({cb_data['size']} bytes):")
                floats = cb_data['floats']
                for i in range(0, min(len(floats), 32), 4):
                    vals = floats[i:i+4]
                    if len(vals) == 4:
                        w(f"      c{i//4}: ({vals[0]:12.4f}, {vals[1]:12.4f}, {vals[2]:12.4f}, {vals[3]:12.4f})")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 5: DEFERRED LIGHTING SYSTEM
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("5. DEFERRED LIGHTING SYSTEM")
    w("=" * 100)
    w()

    sun_draws = [e for e in timeline if e['classification']['pass'] == 'DEFERRED_SUN']
    light_vol_draws = [e for e in timeline if e['classification']['pass'] == 'LIGHT_VOLUME']

    w(f"Deferred sun draws: {len(sun_draws)}")
    w(f"Light volume draws: {len(light_vol_draws)}")

    if sun_draws:
        d = sun_draws[0]
        w(f"\n--- Sun Pass ---")
        w(f"  Draw index: {d['index']}")
        w(f"  Type: {d['type']}, Topology: {d['topology']}")
        w(f"  PS: {d['ps']}  (src={d['classification'].get('ps_source','')} entry={d['classification'].get('ps_entry','')})")
        w(f"  VS: {d['vs']}  (src={d['classification'].get('vs_source','')} entry={d['classification'].get('vs_entry','')})")
        w(f"  PS SRVs:")
        for srv in d.get('psSRVs', []):
            w(f"    slot {srv['slot']}: {srv.get('format','?')} {srv.get('width','?')}x{srv.get('height','?')} ({srv.get('dimension','?')})")
        w(f"  VS SRVs:")
        for srv in d.get('vsSRVs', []):
            w(f"    slot {srv['slot']}: {srv.get('format','?')} {srv.get('width','?')}x{srv.get('height','?')}")

    if light_vol_draws:
        w(f"\n--- Light Volumes ---")
        w(f"  Total: {len(light_vol_draws)} draws")
        lv_ps = Counter(e['ps'] for e in light_vol_draws if e['ps'])
        lv_vs = Counter(e['vs'] for e in light_vol_draws if e['vs'])
        w(f"  Unique PS: {len(lv_ps)}, Unique VS: {len(lv_vs)}")
        for addr, cnt in lv_ps.most_common():
            info = shader_map.get(addr, {})
            w(f"    PS {addr}: {cnt} draws  entry={info.get('entry_point','?')}")
        for addr, cnt in lv_vs.most_common():
            info = shader_map.get(addr, {})
            w(f"    VS {addr}: {cnt} draws  entry={info.get('entry_point','?')}")
        topos = Counter(e['topology'] for e in light_vol_draws)
        w(f"  Topologies: {dict(topos)}")

    # Deferred lighting CBs
    sun_cbs = analyze_cb_patterns(timeline, shader_map, 'DEFERRED_SUN', max_draws=2)
    if sun_cbs:
        w(f"\n--- Deferred Sun CB Data ---")
        for cb_info in sun_cbs:
            for cb_key, cb_data in cb_info['cbs'].items():
                w(f"  {cb_key} ({cb_data['size']} bytes):")
                floats = cb_data['floats']
                for i in range(0, min(len(floats), 64), 4):
                    vals = floats[i:i+4]
                    if len(vals) == 4:
                        w(f"    c{i//4}: ({vals[0]:12.6f}, {vals[1]:12.6f}, {vals[2]:12.6f}, {vals[3]:12.6f})")

    lv_cbs = analyze_cb_patterns(timeline, shader_map, 'LIGHT_VOLUME', max_draws=3)
    if lv_cbs:
        w(f"\n--- Light Volume CB Data (first 3 lights) ---")
        for cb_info in lv_cbs:
            w(f"\n  Draw {cb_info['draw_index']}:")
            for cb_key, cb_data in cb_info['cbs'].items():
                w(f"    {cb_key} ({cb_data['size']} bytes):")
                floats = cb_data['floats']
                for i in range(0, min(len(floats), 32), 4):
                    vals = floats[i:i+4]
                    if len(vals) == 4:
                        w(f"      c{i//4}: ({vals[0]:12.6f}, {vals[1]:12.6f}, {vals[2]:12.6f}, {vals[3]:12.6f})")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 6: SKY, FOG, WATER, FORWARD PASSES
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("6. SKY, FOG, WATER & FORWARD PASSES")
    w("=" * 100)
    w()

    for pass_name in ['SKY', 'FOG', 'WATER', 'FORWARD_BASIC', 'FORWARD_PARTICLES',
                      'FORWARD_BLOOD', 'FULLSCREEN_HDR', 'HDR_UNKNOWN']:
        pass_draws = [e for e in timeline if e['classification']['pass'] == pass_name]
        if not pass_draws:
            continue
        w(f"--- {pass_name} ({len(pass_draws)} draws) ---")
        ps_counts = Counter(e['ps'] for e in pass_draws if e['ps'])
        for addr, cnt in ps_counts.most_common():
            info = shader_map.get(addr, {})
            w(f"  PS {addr}: {cnt} draws  src={info.get('source_name','?')}  entry={info.get('entry_point','?')}")
        vs_counts = Counter(e['vs'] for e in pass_draws if e['vs'])
        for addr, cnt in vs_counts.most_common():
            info = shader_map.get(addr, {})
            w(f"  VS {addr}: {cnt} draws  src={info.get('source_name','?')}  entry={info.get('entry_point','?')}")
        mats = Counter(e['classification'].get('material', '?') for e in pass_draws)
        w(f"  Materials: {dict(mats)}")
        idx_range = f"{pass_draws[0]['index']}-{pass_draws[-1]['index']}"
        w(f"  Draw range: [{idx_range}]")
        w()

    # ═══════════════════════════════════════════════════════════════
    # SECTION 7: POST-PROCESSING CHAIN (per-step)
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("7. POST-PROCESSING CHAIN (every step identified)")
    w("=" * 100)
    w()

    post_pass_names = ['GBUFFER_REPACK', 'STENCIL_MASK', 'LUMINANCE', 'BLOOM',
                       'TONEMAP_COMPOSITE', 'POST_HDR', 'LDR_POST', 'DOF', 'DOF_COC',
                       'HALF_RES_POST', 'FXAA', 'HEAT_HAZE', 'SHARPEN', 'COLOR_GRADE',
                       'INTERIOR_MASK', 'PASSTHROUGH', 'FINAL', 'UI']

    w(f"{'Step':<25} {'Draws':>5} {'Range':<12} {'RT':<40} {'Shader Source':<25} {'Entry Point'}")
    w("-" * 140)

    for pname in post_pass_names:
        pdraws = [e for e in timeline if e['classification']['pass'] == pname]
        if not pdraws:
            continue
        rng = f"{pdraws[0]['index']}-{pdraws[-1]['index']}"
        # Get representative RT
        rt = pdraws[0]['rt_sig'][:40] if pdraws else ''
        ps_entries = set()
        ps_srcs = set()
        for e in pdraws:
            ps_srcs.add(e['classification'].get('ps_source', ''))
            ps_info = shader_map.get(e['ps'], {})
            ps_entries.add(ps_info.get('entry_point', ''))
        src = ', '.join(s for s in sorted(ps_srcs) if s)
        ep = ', '.join(s for s in sorted(ps_entries) if s)
        w(f"{pname:<25} {len(pdraws):>5} {rng:<12} {rt:<40} {src:<25} {ep}")

    # Per-step details
    w()
    w("--- Per-Step Details ---")
    for pname in post_pass_names:
        pdraws = [e for e in timeline if e['classification']['pass'] == pname]
        if not pdraws:
            continue
        w(f"\n  {pname}:")
        for e in pdraws:
            info = shader_map.get(e['ps'], {})
            w(f"    Draw {e['index']:>4}: {e['type']:<20} PS={e['ps'] or 'none'} "
              f"src={info.get('source_name','')} entry={info.get('entry_point','')} "
              f"RT={e['rt_sig'][:60]}")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 8: COMPLETE SHADER DATABASE
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("8. COMPLETE SHADER DATABASE (every shader mapped to source)")
    w("=" * 100)
    w()

    shader_db = build_full_shader_database(timeline, shader_map)

    # Group by source file
    by_source = defaultdict(list)
    for addr, info in shader_db.items():
        by_source[info['source_name']].append(info)

    w(f"Total shaders in frame: {len(shader_db)}")
    w(f"Mapped to source: {sum(1 for v in shader_db.values() if v['source_name'] != 'UNKNOWN')}")
    w(f"Unmapped: {sum(1 for v in shader_db.values() if v['source_name'] == 'UNKNOWN')}")
    w()

    w(f"{'Source File':<30} {'Type':>4} {'Shaders':>8} {'Draws':>6} {'Passes'}")
    w("-" * 100)

    for source in sorted(by_source.keys()):
        shaders = by_source[source]
        for stype in ['ps', 'vs', 'hs', 'ds']:
            typed = [s for s in shaders if s['type'] == stype]
            if not typed:
                continue
            total_draws = sum(s['draw_count'] for s in typed)
            all_passes = set()
            for s in typed:
                all_passes.update(s['passes'])
            w(f"{source:<30} {stype:>4} {len(typed):>8} {total_draws:>6} {', '.join(sorted(all_passes))}")

    # Unmapped shaders detail
    unmapped = [v for v in shader_db.values() if v['source_name'] == 'UNKNOWN']
    if unmapped:
        w(f"\n--- Unmapped Shaders ({len(unmapped)}) ---")
        for s in sorted(unmapped, key=lambda x: -x['draw_count']):
            w(f"  {s['address']} ({s['type']}): {s['draw_count']} draws in passes {sorted(s['passes'])}")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 9: STATE CHANGE ANALYSIS
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("9. STATE CHANGE ANALYSIS")
    w("=" * 100)
    w()

    state_stats = analyze_state_changes(timeline)
    total = state_stats['total_draws']
    w(f"Total draws: {total}")
    w(f"PS changes: {state_stats['ps_changes']} ({100*state_stats['ps_changes']/max(1,total-1):.1f}% of transitions)")
    w(f"VS changes: {state_stats['vs_changes']} ({100*state_stats['vs_changes']/max(1,total-1):.1f}%)")
    w(f"RT changes: {state_stats['rt_changes']} ({100*state_stats['rt_changes']/max(1,total-1):.1f}%)")
    w(f"SRV changes: {state_stats['srv_changes']} ({100*state_stats['srv_changes']/max(1,total-1):.1f}%)")
    w(f"Redundant PS binds: {state_stats['redundant_ps']}")
    w(f"Redundant VS binds: {state_stats['redundant_vs']}")

    # Per-pass state changes
    w(f"\n--- State Changes by Pass ---")
    for p in passes:
        if len(p['draws']) < 2:
            continue
        ps_changes = sum(1 for i in range(1, len(p['draws']))
                        if p['draws'][i]['ps'] != p['draws'][i-1]['ps'])
        vs_changes = sum(1 for i in range(1, len(p['draws']))
                        if p['draws'][i]['vs'] != p['draws'][i-1]['vs'])
        w(f"  {p['pass_name']:<25} draws={len(p['draws']):>5}  PS changes={ps_changes:>4}  VS changes={vs_changes:>4}")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 10: RESOURCE INVENTORY
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("10. RESOURCE INVENTORY (all SRVs bound during frame)")
    w("=" * 100)
    w()

    resources = build_resource_inventory(timeline)
    sorted_resources = sorted(resources.items(), key=lambda x: -x[1]['bind_count'])

    w(f"{'Format':<25} {'Size':<15} {'Dimension':<15} {'Binds':>6} {'Consumers'}")
    w("-" * 120)
    for key, info in sorted_resources:
        size = f"{info['width']}x{info['height']}" if info['width'] else '?'
        consumers = ', '.join(sorted(info['consumers']))
        w(f"{info['format'] or '?':<25} {size:<15} {info['dimension']:<15} {info['bind_count']:>6} {consumers}")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 11: CUBEMAP PASS ANALYSIS
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("11. CUBEMAP / REFLECTION PROBE ANALYSIS")
    w("=" * 100)
    w()

    cube_draws = [e for e in timeline if e['classification']['pass'] == 'CUBEMAP']
    if cube_draws:
        cube_mats = Counter(e['classification'].get('material', '?') for e in cube_draws)
        w(f"Total cubemap draws: {len(cube_draws)}")
        w(f"\nMaterial breakdown:")
        for mat, count in cube_mats.most_common():
            pct = 100 * count / max(1, len(cube_draws))
            w(f"  {mat:<35} {count:>5} ({pct:.1f}%)")

        cube_ps = set(e['ps'] for e in cube_draws if e['ps'])
        gb_ps = set(e['ps'] for e in gbuffer_draws if e['ps'])
        shared = cube_ps & gb_ps
        w(f"\nShared PS with GBuffer: {len(shared)}/{len(cube_ps)}")
        cube_only = cube_ps - gb_ps
        if cube_only:
            w(f"Cubemap-only PS ({len(cube_only)}):")
            for addr in sorted(cube_only):
                info = shader_map.get(addr, {})
                w(f"  {addr}: src={info.get('source_name','?')} entry={info.get('entry_point','?')}")
    else:
        w("No cubemap pass in this capture.")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 12: CROSS-CAPTURE SHADER CONSISTENCY
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("12. CROSS-CAPTURE ANALYSIS")
    w("=" * 100)
    w()

    # Build per-capture pass structure
    all_pass_sets = []
    all_material_sets = []
    capture_shader_counts = []

    for cap in all_captures:
        if not cap['frames']:
            continue
        tl = build_draw_timeline(cap['frames'][0], shader_map)
        ps_in_frame = group_into_passes(tl)
        pass_names = set(p['pass_name'] for p in ps_in_frame)
        all_pass_sets.append(pass_names)

        gb = [e for e in tl if e['classification']['pass'] == 'GBUFFER']
        mats = set(e['classification'].get('material', '?') for e in gb)
        all_material_sets.append(mats)

        ps_addrs = set(e['ps'] for e in tl if e['ps'])
        vs_addrs = set(e['vs'] for e in tl if e['vs'])
        capture_shader_counts.append({
            'name': cap['name'],
            'draws': len(tl),
            'passes': len(ps_in_frame),
            'pass_names': pass_names,
            'gb_draws': len(gb),
            'gb_materials': mats,
            'unique_ps': len(ps_addrs),
            'unique_vs': len(vs_addrs),
        })

    if all_pass_sets:
        common_passes = set.intersection(*all_pass_sets)
        all_passes_union = set.union(*all_pass_sets)
        w(f"Passes in ALL captures: {sorted(common_passes)}")
        w(f"Passes in SOME captures: {sorted(all_passes_union - common_passes)}")

    if all_material_sets:
        common_mats = set.intersection(*all_material_sets)
        all_mats = set.union(*all_material_sets)
        w(f"\nGBuffer materials in ALL captures: {sorted(common_mats)}")
        w(f"GBuffer materials in SOME captures: {sorted(all_mats - common_mats)}")

    w(f"\n{'Capture':<55} {'Draws':>5} {'Passes':>6} {'GBuf':>5} {'Materials':>9} {'PS':>4} {'VS':>4}")
    w("-" * 100)
    for cs in capture_shader_counts:
        w(f"{cs['name']:<55} {cs['draws']:>5} {cs['passes']:>6} {cs['gb_draws']:>5} {len(cs['gb_materials']):>9} {cs['unique_ps']:>4} {cs['unique_vs']:>4}")

    # ═══════════════════════════════════════════════════════════════
    # SECTION 13: IMPLEMENTATION REFERENCE
    # ═══════════════════════════════════════════════════════════════
    w()
    w("=" * 100)
    w("13. IMPLEMENTATION REFERENCE — WHERE TO INJECT")
    w("=" * 100)
    w()

    # Find key injection points
    if sun_draws:
        sun_idx = sun_draws[0]['index']
    else:
        sun_idx = '?'
    gb_first = gbuffer_draws[0]['index'] if gbuffer_draws else '?'
    gb_last = gbuffer_draws[-1]['index'] if gbuffer_draws else '?'
    sh_first = shadow_draws[0]['index'] if shadow_draws else '?'
    sh_last = shadow_draws[-1]['index'] if shadow_draws else '?'
    lv_first = light_vol_draws[0]['index'] if light_vol_draws else '?'
    lv_last = light_vol_draws[-1]['index'] if light_vol_draws else '?'

    w("Key injection points for feature implementation:")
    w()
    w(f"  SSAO/SSIL:          After deferred sun (draw {sun_idx}), before light volumes")
    w(f"  SSS (subsurface):   After deferred sun, needs skin material mask from GBuffer")
    w(f"  Shadow replacement: Replace draws [{sh_first}-{sh_last}] or intercept shadow RT bind")
    w(f"  RT shadows:         After GBuffer [{gb_first}-{gb_last}], before deferred lighting")
    w(f"  Voxel GI:           Re-render GBuffer geometry into voxel grid before deferred")
    w(f"  Tessellation:       Intercept GBuffer draws, add HS/DS for displacement")
    w(f"  Material upgrade:   Replace GBuffer PS (per source file: objects, terrain, skin, etc.)")
    w(f"  Bloom replacement:  Replace bloom chain draws")
    w(f"  Tonemapping:        Replace composite draw")
    w(f"  FXAA replacement:   Replace final output draws")
    w()

    w("Material source files to patch for each feature:")
    w()
    w("  Subsurface Scattering:")
    w("    - skin.hlsl (character skin)")
    w("    - character.hlsl (full character body)")
    w("    - Need to encode SSS profile ID in GBuffer (RT1.A currently has emissive/translucency)")
    w()
    w("  Better Normals:")
    w("    - objects.hlsl (all static objects)")
    w("    - terrainfp4.hlsl (terrain)")
    w("    - triplanar.hlsl (rocks/cliffs)")
    w("    - skin.hlsl, character.hlsl (characters)")
    w()
    w("  Displacement/Tessellation:")
    w("    - terrain.hlsl / terrainfp4.hlsl (add HS/DS)")
    w("    - objects.hlsl (add HS/DS for detailed objects)")
    w("    - rtwtessellator.hlsl already exists for shadow tessellation — reuse pattern")
    w()
    w("  Water Improvement:")
    w("    - water.hlsl (flow maps, reflection, scum, rain)")
    w("    - Forward pass, not in GBuffer — has own PBR lighting")
    w()
    w("  Sky Improvement:")
    w("    - SkyX_Skydome.hlsl (atmospheric scattering)")
    w("    - SkyX_Clouds.hlsl, SkyX_VolClouds.hlsl")
    w()

    # Write report
    report = '\n'.join(lines)
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(report)
    print(report.encode('ascii', errors='replace').decode('ascii'))
    print(f"\nReport written to: {output_path}")


# ── Main ─────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: survey_deep_analyze.py <survey_root_dir> [output_report_path]")
        sys.exit(1)

    survey_root = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        'docs', 'pipeline_deep_analysis.txt')

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

    print(f"\nBuilding shader source map from {len(shader_dirs)} shader directories...")
    shader_map = build_shader_source_map(shader_dirs)
    print(f"  Mapped {len(shader_map)} shader addresses to source files")

    mapped = sum(1 for v in shader_map.values() if v.get('source_name'))
    unmapped = len(shader_map) - mapped
    print(f"  With source: {mapped}, Unknown: {unmapped}")

    # Show source distribution
    source_counts = Counter(v.get('source_name', 'UNKNOWN') for v in shader_map.values())
    print(f"\n  Source files ({len(source_counts)}):")
    for src, cnt in source_counts.most_common():
        print(f"    {src}: {cnt} shaders")

    print(f"\nGenerating exhaustive analysis report...")
    generate_report(captures, shader_map, output_path)


if __name__ == '__main__':
    main()
