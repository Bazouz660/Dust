# DLSS / FSR Temporal Upscaling — Design

Status: **DESIGN** · Branch: `feature/upscaling` (off `main`) · Started 2026-07-08

Goal: add vendor-selectable temporal super-resolution / DLAA to Dust —
DLSS on NVIDIA RTX, FSR on everything else — for both image quality (DLAA)
and, later, framerate (upscaling from a lower internal resolution).

---

## 1. Why this is viable now (when TAA wasn't)

Dust's custom temporal AA was abandoned 2026-06-21 (see the SMAA-temporal
post-mortem) because Kenshi exposes **no engine motion vectors**, and a
whole-frame history clamp had no way to say "don't trust this pixel." Grass —
wall-to-wall in Kenshi, content that changes frame-to-frame independent of the
camera — either ghosted (reprojected) or flickered (clamped).

DLSS-SR / FSR2 change the calculus because they have first-class handling for
content you *can't* produce motion vectors for: the **reactive mask** and the
**transparency & composition mask**. Tag those pixels and the upscaler falls
back toward the current frame there — no ghosting, just reduced temporal
benefit. That escape hatch is the difference between "impossible" and "shipping
AAA technique." The plan is therefore **not** "produce perfect MVs for all of
Kenshi" (grass can't be replayed) — it is "produce MVs where we can, mask
honestly where we can't."

---

## 2. Hard constraint: Kenshi is D3D11

The modern upscalers are DX12/Vulkan-first. What actually runs in a D3D11 title:

| Feature | D3D11 Kenshi | Notes |
|---|---|---|
| DLSS Super Resolution / DLAA (incl. DLSS 4 transformer model) | ✅ native | Streamline / NGX have a D3D11 path; RTX 20-series+ |
| DLSS Frame Generation (DLSS-G) | ❌ | Needs DX12/VK swapchain |
| FSR 2 upscaler | ✅ native | Compute-only; community D3D11 backends exist; all GPUs |
| FSR 3.1 upscaler | ⚙️ via D3D11-on-12 interop | Officially DX12/VK; reachable through a shared D3D12 device |
| FSR 4 / 4.1 | ❌ effectively | **RDNA4-only + DX12-only**; reached only through the FSR 3.1 API auto-upgrading on RX 9000 hardware |
| FSR 3 Frame Generation | ❌ | Needs a DX12/VK proxy swapchain |

**Implications baked into this design:**
- Native D3D11 backends first: **DLSS-SR (NVIDIA)** + **FSR 2 (all GPUs)**.
- "Latest AMD" (FSR 3.1 → FSR 4 on RDNA4) requires a **D3D11-on-12 interop
  layer** (`ID3D11On12Device`, shared resources, run the upscaler on a D3D12
  device, copy back). This is a later, opt-in backend — the same approach
  OptiScaler uses to bring modern upscalers into DX11 games. Study OptiScaler
  as prior art.
- **Frame Generation is out of scope** for D3D11 without a full DX12 proxy
  swapchain. Not in this plan.

---

## 3. The upscaler input contract → where each input comes from

DLSS-SR, FSR2, and FSR3.1 take near-identical inputs. Mapped to Dust:

| Input | Source in Dust | Status |
|---|---|---|
| Color (jittered, low or native res) | `hdr_rt` / `ldr_rt` | have |
| Depth | `DUST_RESOURCE_DEPTH` (R32F, linear viewZ/far) | have |
| Camera jitter offset | host viewport jitter (`SetTemporalJitterEnabled`, RSSetViewports hook) | **already built** — v0.8.0 `c5b8752` / stash; worked in-game |
| Camera / static motion vectors | depth + prev/cur view-proj (RTGI/SSAO reprojection math) | derivable, no geometry needed |
| Animated-object motion vectors | `GeometryReplay` velocity pass + cached prev-frame bone cbuffers | infra on dxr-experiment; **not yet ported to main** |
| Foliage / water / particle MVs | — impossible | → **reactive mask** |
| Exposure (optional) | auto-exposure or from tonemapper | nice-to-have |
| Texture mip bias `log2(renderScale)` | `CreateSamplerState` hook (pattern already used for `CreateTexture2D`) | Milestone B only |

Motion-vector convention differs per SDK (DLSS: pixel-space, top-left origin;
FSR: NDC or pixel depending on flags). The MV pass writes a canonical
screen-space UV-delta buffer; each backend adapts at bind time.

---

## 4. Architecture

A thin resolver interface, GPU-selected at runtime:

```
IUpscaler
  bool   Init(device, displayW, displayH, renderW, renderH, qualityMode)
  void   Evaluate(color, depth, motionVectors, reactiveMask, jitterXY, out)
  void   Shutdown()

  DLSSUpscaler   — Streamline/NGX, native D3D11         (NVIDIA RTX only)
  FSRUpscaler    — FSR2 native D3D11                     (all GPUs)
                 — FSR3.1 via D3D11-on-12 interop        (opt-in; FSR4 on RDNA4)
```

Backend availability is filtered by detected adapter (vendor ID + arch):
- NVIDIA RTX  → DLSS offered (+ FSR as fallback)
- AMD / Intel / older NV → FSR only; FSR3.1/4 path shown only when the interop
  layer + capable hardware are present.

~90% of the work (jitter, MV pass, masks, resource tagging, resolution plumbing)
is backend-agnostic. The backend is just the resolver call.

### Shared pipeline (host side, per frame, temporal on)
1. Apply sub-pixel camera jitter (existing viewport-jitter hook), record `jitterXY`.
2. Produce the **motion-vector RT** (§5).
3. Produce the **reactive mask** (§6).
4. Call `IUpscaler::Evaluate` before UI/overlay composition, writing to the
   display-res target.
5. Advance history (prev view-proj, prev bone cbuffers for A1).

---

## 5. Motion vectors — TRUE per-object MVs (no camera-only stage)

Decision (user, 2026-07-08): **no camera-only MV stage.** Camera-only reprojection
treats every independently-moving agent (walking characters, animals, doors) as
static → they ghost. Kenshi is full of such agents, so we build true per-object
MVs from the start.

**Velocity G-buffer via geometry replay:**
1. **Capture** each GBuffer draw's transform state (GeometryCapture already
   snapshots: `cbStagingCopy` = clip matrix; `cbCopies` = skinned bone palette;
   `instVBCopy` = per-instance transforms — ~80% of the machinery exists).
2. **Persist + match** frame N−1 → N. The capture is currently single-frame
   (`ResetFrame` clears all, no identity) — this is the piece to add:
   double-buffer captured transforms + a conservative frame-to-frame key.
3. **Velocity VS variants** (static / skinned / instanced) authored from the
   readable game OGRE VS sources + auto-param layout, injected via ShaderPatch.
   Each rasterizes at CURRENT clip pos (SV_Position) and outputs PREVIOUS clip
   pos as an interpolant → `MV = cur.xy/cur.w − prev.xy/prev.w`.
4. Rasterize with `depth_func = EQUAL` against the real GBuffer depth (blood-pass
   trick) so MVs land exactly on the shaded pixels.
5. **Resolve + dilate** into the velocity RT.

**Robustness principle — conservative match, safe fallback:**
- Confident frame-to-frame match → emit true MV.
- No confident match (new object / disocclusion / ambiguous) → **zero MV +
  reactive mask**, NEVER a wrong MV. A mismatch degrades to "no temporal benefit
  here," not "smear." This asymmetry is what makes it robust.

**Permanent exception:** wind-animated grass + GPU particles have no CPU-side
previous state (grass sway is computed in-VS from a time uniform) and no stable
per-instance identity → cannot get true MVs by replay. → reactive mask (§6).
Everything with a reconstructable transform (static, rigid props, skinned
characters/animals) gets true MVs.

**Draw identity is the crux** — validated by the A-spike (2026-07-08, survey
capture + tools/analyze_mv_identity.py, stillcam + pancam @ DetailLevel 3).

**Spike result: GO.**
- GBuffer pass cleanly isolated (2× B8G8R8A8 + R32F @ native, D24 depth;
  ~1480 draws/frame). Shadow atlas (12288² R32F) correctly excluded.
- **Draw order/count is highly stable** frame-to-frame — nearly every VS class
  holds a constant draw count across all 8 frames; unstable ones vary ±2–4.
  → order-based matching within a class is viable and robust for the vast
  majority of draws. (OGRE renders a deterministic queue.)
- Only **~16 distinct GBuffer vertex shaders** → velocity VS variants tractable.

**Transform-source taxonomy (read from the dumped shaders — the load-bearing
finding). NO category hides its transform in an opaque SRV; all three live in
places GeometryCapture already snapshots:**

| Class | Category | Transform source | Snapshot |
|---|---|---|---|
| `0x55144CF8` | Skinned char/creature | bone palette `worldMatrix3x4Array` (CB) + viewProj (CB) | `cbCopies` |
| `0x55028DB8` family | Instanced static | per-instance matrix in instance VB (slot 1) + worldViewProj (CB) | `instVBCopy` |
| `0x54E54FF8` e1 | Static rigid | worldMatrix + worldViewProj (CB) | `cbStagingCopy` |
| `0x54E54FF8` e3 | Foliage/plants | worldMatrix centre (CB) + per-plant idx + wind `data1` | CB + prev-time |

Caveats: (a) analyzer "moved/matched" is a blunt proxy — view-matrix FP jitter
(phantom-motion, ~0.5–2 L2 on everything even still) + skinned anim + shared
instanced CBs are conflated; trust order/count stability, not the moved counts.
(b) Instanced geometry needs per-*instance* order stability within a batch
(survey is per-draw, can't confirm) — the one medium risk; mitigated by reactive
fallback.

---

## 6. Reactive / transparency mask

The load-bearing safety net. Marks pixels the upscaler must not trust history
for = the un-matchable set: grass/foliage, GPU particles, transparents, and any
draw that failed confident frame-to-frame matching this frame (§5). Those pixels
get current-frame resolve — no ghosting, just no extra temporal AA.

Sources for the mask: the per-draw "no confident match" flag from the MV pass,
GBuffer material tags already used elsewhere (e.g. the subsurface/skin `buf1.a`
tag), the alpha/transparent draw stream, and the foliage/grass draw
classification from the tess/geometry work.

---

## 7. Milestones

| # | Deliverable | Risk retired |
|---|---|---|
| **A-spike** | Identity-stability measurement: port capture to this branch, log confident-match rate per category (static/skinned/instanced/foliage) over real gameplay, per candidate key | **Is clean/robust MV even achievable, and for which content + by what key** — de-risks the whole feature before pipeline build |
| **A** | True velocity G-buffer: double-buffer + match, velocity VS variants, depth=EQUAL rasterize, resolve+dilate, **MV debug viz**, reactive mask | Per-object motion vectors correct in-game (verified via debug viz before any resolver) |
| **B** | Wire MV + depth + jitter into DLSS-SR + FSR2 as **DLAA** (native res) | SDK integration, jitter, resolve pipeline; shippable AA result |
| **C** | Upscaling for FPS — force lower internal render resolution | OGRE RTT/viewport hook vs GBuffer/RTGI/SSAO/shadow/tess assumptions |
| **D** (later) | FSR 3.1 via D3D11-on-12 interop → FSR 4 on RDNA4 | DX12 interop layer |
| out | Frame Generation | needs DX12 proxy swapchain — not planned |

**A-spike is the de-risking gate.** Draw identity across frames is the crux of
"clean and robust"; measure it empirically before committing to the full
pipeline. The velocity G-buffer (A) must be verified via debug visualization
(flat where still, smooth field tracking motion) BEFORE any upscaler consumes it
— the explicit lesson from the abandoned TAA work.

---

## 8. Open questions / to verify during A0

- Streamline vs raw NGX for the DLSS D3D11 path (Streamline is the modern,
  supported route; confirm its D3D11 SR support level and redistributable size).
- FSR2 D3D11 backend: adopt a community backend vs port the reference compute
  backend to D3D11.
- Where to run the resolve in the frame (before/after Dust's LUT + UI). LUT is
  display-referred straight to UNORM (no OETF) — decide whether the upscaler
  sees pre- or post-tonemap color; DLSS prefers pre-tonemap HDR-ish input.
- Licensing/redist: DLSS (NVIDIA RTX SDKs License, ship `nvngx_dlss.dll` /
  Streamline dlls), FSR2 (MIT). RESOLVED: the mod is GPLv3 + a section-7
  linking exception (see LICENSE) that permits combining/conveying with the
  DLSS and FSR SDKs -- FSR (MIT) is GPL-compatible outright; DLSS's proprietary
  license forbids copyleft, so the exception is what makes the combined
  distribution valid. Still owed to NVIDIA regardless of the mod license:
  attribution/NVIDIA marks in the about/credits, pre-release notification
  (developer.nvidia.com/sw-notification), ship only the redistributable runtime
  DLL, and do NOT commit the whole SDK to a public repo (NVIDIA sec. 4b:
  no stand-alone SDK redistribution).
