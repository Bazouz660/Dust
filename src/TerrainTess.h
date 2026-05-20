#pragma once

#include <d3d11.h>
#include <atomic>
#include <cstdint>

// Terrain tessellation. Inserts a Hull/Domain shader pair on TEXTURED GBuffer
// terrain draws to subdivide and displace vertices. Displacement = the PS's
// visible luminance (BLEND0+1+2+3 chain), computed in the DS by replicating
// the PS's exact composition — so DS output equals what the PS draws → no
// chunk seams.
namespace TerrainTess
{
    // Tunable parameters exposed to the GUI. Mirrored into a HLSL cbuffer
    // each Begin() so changes take effect on the next draw.
    //
    // Layout:
    //   22 plugin-set floats (sent via SetTerrainTessControls) + 2 pad
    //   12 host-set floats (per-PS BLEND# channel masks)
    // Total 36 floats = 144 bytes (9 float4 rows).
    //
    // Displacement formula (DS): 4-tap multi-band bandpass with frequency-falloff
    // curve so dune-scale bumps register and high-freq bumps get less amplitude.
    //   v0..v3  = ComputePsLum at mips sharpMip, +2, +4, +8
    //   slice_hi  = v0 - v1   (high-freq band — spikes)
    //   slice_mid = v1 - v2   (mid-freq band — most bumps)
    //   slice_lo  = v2 - v3   (low-freq band — dune-scale features)
    //   keep    = smoothstep(scale*0.5, scale, |slice_mid|)            (spike gate)
    //   bp      = (slice_lo + slice_mid * hfWeight + slice_hi * hfWeight² * keep) / refLum
    //   shaped  = bp / (|bp| + scale)                                  (soft saturation)
    //   h       = (shaped + bias) * amp
    struct Controls
    {
        float maxFactor       = 16.0f;
        // Distance thresholds below are now WORLD UNITS from the camera
        // (Kenshi unit ~cm). Tess fade now uses cameraPos from PS cb0,
        // not clip-space w (which was broken: negative for visible terrain).
        float factFadeStart   = 8000.0f;
        float factFadeEnd     = 30000.0f;
        float amplitude       = 4.0f;
        float ampFadeStart    = 8000.0f;
        float ampFadeEnd      = 30000.0f;
        float ampFadeEnabled  = 1.0f;
        // 0 = off, 1 = PS visible-luminance grayscale, 2 = DS-replica diff overlay.
        float debugViewMode   = 0.0f;
        // Pure additive output offset on the saturated signal — independent of amp.
        float displacementBias = 0.0f;
        float factorSnapStep   = 6.0f;
        // Blend per-vertex normal (0) → world-up (1) for displacement direction.
        // 1 fixes seams from boundary normal mismatches.
        float dispDirWorldUp   = 0.0f;
        // 0 = off, 1 = tess wireframe, 2 = vanilla mesh wireframe (no tess),
        // 3 = strip→list IB conversion + TRIANGLELIST + no tess (isolates IB conversion).
        float wireframe        = 0.0f;
        // Mip level for the SHARP bandpass tap. Higher = blurrier sharp tap →
        // fewer high-freq spikes. Mid tap is fixed at +2, blurry at +4.
        float sharpMip         = 0.0f;
        // Saturation knee for the soft-saturation curve. Smaller = more
        // equalized magnitude across textures (bumpy and subtle textures both
        // produce similar displacement). Larger = more dynamic range preserved.
        float scale            = 0.04f;
        // Frequency-falloff weight on the high-freq slice (slice_hi = sharp - mid).
        // 1.0 = flat response (high-freq bumps full amplitude), 0 = mid-band
        // only (high-freq bumps killed). Forms a 2-point falloff curve over
        // frequency. Independent of the spike gate.
        float hfWeight         = 1.0f;
        // LF-aware spike cap. DS computes h with both full and LF-only
        // pipelines; soft-caps the (h_full − h_lf) excess. 0 = off.
        float spikeCap         = 4.0f;
        // Per-slice mip offsets ("smoothness per band"). Each band has its
        // own tap pair; raising one blurs that band without touching the
        // others. 0 = current behavior (tight contiguous bands).
        // Band ranges (with offsets = 0): hi=K..K+1, hm=K+1..K+2,
        //                                 mid=K+2..K+4, lo=K+4..K+8.
        float smoothHi         = 6.0f;  // slice_hi tap pair (K..K+1)
        float smoothHiMid      = 6.0f;  // slice_hm tap pair (K+1..K+2)
        float smoothMid        = 0.0f;  // slice_mid tap pair (K+2..K+4)
        float smoothLo         = 2.0f;  // slice_lo tap pair (K+4..K+8)
        // Per-tap distance cutoffs. The 8-tap multi-band bandpass is the
        // dominant per-DS-vertex GPU cost (56 texture samples). Past these
        // clip-space-w distances we drop high-frequency bands to save work,
        // since fine detail isn't visible at distance anyway. Tuning:
        //   - farHi  : past this, drop slice_hi + slice_hm (lose finest rocks)
        //   - farMid : past this, drop slice_mid too (LF dune surface only)
        // slice_lo is always kept until the amp-fade early-out kicks in.
        float farHi            = 5000.0f;
        float farMid           = 15000.0f;
        // CPU-side distance threshold in *abs(clip-space w)* units. When
        // > 0, terrain chunks with abs(WVP[3][3]) > skipDistance bypass
        // the HS/DS pipeline entirely (the original DrawIndexed runs
        // direct) — recovers ~5ms of fixed tess-pipeline overhead per
        // distant chunk. 0 disables. Scale is whatever Kenshi's clip-space
        // w is in — tens of thousands per visible chunk in practice, so
        // typical good values are in the 30000-100000 range. Use the
        // "WVP-W sample" log lines (Logging=1) to calibrate.
        float skipDistance     = 0.0f;
        // Pad to keep the 3 mask float4s on 16-byte HLSL cbuffer rows.
        float _farPad1         = 0.0f;
        // Per-PS BLEND# channel selectors. Each PS variant uses BLEND1/2/3
        // #defines to pick a channel (0..3 = R/G/B/A) of blendMap. Host fills
        // these per-draw based on the PS's captured defines (see
        // OnTerrainPsCompiled). (1,0,0,0) = R, (0,1,0,0) = G, etc.
        // All-zero = layer inactive; the BLEND chain skips it.
        float blend1Mask[4]    = { 0, 0, 0, 0 };
        float blend2Mask[4]    = { 0, 0, 0, 0 };
        float blend3Mask[4]    = { 0, 0, 0, 0 };
    };

    Controls* GetControls();

    // Mark the GPU control cbuffer dirty so the next Begin() re-uploads it.
    // Host must call after writing to *GetControls() (e.g., the plugin's
    // SetTerrainTessControls push). The cbuffer is only re-uploaded when
    // dirty, saving a Map+memcpy+Unmap on every terrain draw (the cbuffer
    // contents only change when the GUI changes a setting or a different
    // terrain PS becomes bound).
    void MarkControlsDirty();

    // Master enable/disable. When off, the DrawIndexed hook short-circuits to
    // the original draw without entering TryDrawTessellated at all.
    //
    // gEnabledFlag is exposed so GetEnabled() can be inlined at hot-path call
    // sites (HookedDrawIndexed). Do not touch it directly — always go through
    // SetEnabled to keep host/plugin state in sync.
    namespace detail { extern bool gEnabledFlag; }
    inline bool GetEnabled() { return detail::gEnabledFlag; }
    void  SetEnabled(bool enabled);

    // GPU time consumed by terrain tess draws on the most recent frame (ms).
    // 0 if no tess this frame, or if the timestamp data isn't ready yet
    // (queries lag by ~1 frame).
    float GetGpuTimeMs();

    // Called by D3D11Hook::ResetFrameState at the start of each frame —
    // closes the previous frame's disjoint timestamp query and harvests
    // the prior frame's results into GpuTimeMs.
    void OnFrameEnd();

    // Compiles the HS/DS. Called once at device capture.
    void Init(ID3D11Device* device);

    // Releases compiled shaders and any cached state.
    void Shutdown();

    // Function-pointer signature matching ID3D11DeviceContext::DrawIndexed.
    typedef void (STDMETHODCALLTYPE* DrawIndexedFn)(
        ID3D11DeviceContext* ctx, UINT indexCount,
        UINT startIndex, INT baseVertex);

    // Try to render a draw call with tessellation. Returns true if the
    // function handled the draw entirely (caller MUST NOT call drawFn again).
    // Returns false for non-terrain draws — caller should do the standard draw.
    //
    // TRIANGLELIST terrain draws: overrides topology to PATCHLIST and routes
    // through HS/DS.
    // TRIANGLESTRIP terrain draws: converts source IB to a list IB, binds it,
    // routes through HS/DS, restores the original IB binding before returning.
    bool TryDrawTessellated(ID3D11DeviceContext* ctx,
                            UINT indexCount, UINT startIndex,
                            INT baseVertex, DrawIndexedFn drawFn);

    // Diagnostic: reflect a shader's I/O signatures and log them. Used to
    // verify VS-HS-DS-PS semantic chains.
    void LogShaderSignature(const void* bytecode, size_t size, const char* tag);

    // Called from CreatePixelShader hook. Looks up the BLEND-define record
    // (stashed earlier by OnTerrainPsCompiled) and associates it with the PS
    // pointer for per-draw mask lookup; also reflects the PS to determine
    // the cb0 BLEND level (number of layers active).
    void OnPixelShaderCreated(const void* bytecode, size_t size, ID3D11PixelShader* ps);

    // Called from CreateVertexShader hook. Reflects to find the byte offset
    // of `worldViewProjMatrix` inside the bound cbuffer slot. We use this in
    // TryDrawTessellated to grab the matrix's translation column from the
    // shadowed cb (see OnContextMap/OnContextUnmap) and decide whether the
    // chunk is so far that the HS/DS pipeline can be skipped entirely.
    void OnVertexShaderCreated(const void* bytecode, size_t size, ID3D11VertexShader* vs);

    // Latest camera world position observed during terrain draws this frame
    // (read out of the terrain PS's $Globals.cameraPos uniform via the
    // cbuffer shadow). Returns true and writes (x,y,z) into `outXYZ` when
    // a recent value is available; false otherwise (no terrain has been
    // drawn yet this session). Backs the DustHostAPI::GetCameraPos call so
    // plugins can read it too.
    bool GetCameraPos(float outXYZ[3]);

    // Called from PSSetShader / VSSetShader hooks. Maintains a cached
    // "is the currently-bound VS+PS a terrain pair" bool so the per-draw
    // check in TryDrawTessellated becomes a single load instead of two
    // COM calls + several map lookups. Big win for non-terrain draws,
    // which still flow through the DrawIndexed hook on every frame.
    void OnPsBound(ID3D11PixelShader* ps);
    void OnVsBound(ID3D11VertexShader* vs);

    // Cached fast path equivalent to "current VS & PS are both terrain
    // and PS has BLEND level >= 0". Updated by OnPsBound/OnVsBound; the
    // bool lives in the namespace so callers can read it inline.
    namespace detail { extern bool gIsTerrainBoundFlag; }
    inline bool IsTerrainBound() { return detail::gIsTerrainBoundFlag; }

    // Same idea for the blood-on-terrain pass: VS = no-TEXTURED terrain VS,
    // PS = blood PS (identified by `bloodTex` bound resource). Set true,
    // TryDrawTessellated re-applies the same displacement to the blood mesh
    // so its depth matches the displaced GBuffer terrain (which uses
    // depth_func=equal — without matching depth, blood is invisible on every
    // tessellated pixel).
    namespace detail { extern bool gIsBloodBoundFlag; }
    inline bool IsBloodBound() { return detail::gIsBloodBoundFlag; }

    // Inline bloom-filter test exposed for D3D11Hook to bail before paying
    // the function-call cost into OnContextMap/Unmap for buffers we never
    // track. Kenshi fires ~4000 Map/Unmaps per frame and >99% are for
    // resources outside our tracking set; this lets the hook bail with a
    // single atomic load + bit test, no function call into our code.
    // 65536-bit (8 KB) single-hash bloom. Sized for ~5000 tracked entries
    // (terrain IBs + staging buffers + cbs) at ~7% FPR — most Map/Unmap
    // bloom tests bail with one atomic load. Smaller blooms saturate to
    // near-100% FPR in busy Kenshi scenes, defeating the early-out — the
    // entire point of the filter.
    namespace detail { extern std::atomic<uint64_t> gTrackedBloom[1024]; }
    inline bool IsResourceTracked(void* p)
    {
        uintptr_t x = (uintptr_t)p;
        x ^= x >> 16;
        x *= 0x85ebca6bULL;
        x ^= x >> 13;
        uint32_t h = (uint32_t)(x & 65535);  // 16 bits → 65536 positions
        return (detail::gTrackedBloom[h >> 6].load(std::memory_order_relaxed) &
                (uint64_t(1) << (h & 63))) != 0;
    }

    // Called from the existing HookedMap / HookedUnmap entry points in
    // D3D11Hook. Used to maintain a CPU-side shadow of VS cbuffer contents
    // so TryDrawTessellated can read the WVP matrix without a GPU readback.
    void OnContextMap  (ID3D11Resource* res, void* mappedPtr);
    void OnContextUnmap(ID3D11Resource* res);

    // Called from HookedCreateBuffer for any VB with initial data. We
    // cache the first vertex's position (float3 at offset 0) by VB
    // pointer so the CPU-side skip can hit the cache instead of doing
    // a CopyResource+Map(READ) at draw time.
    void OnVertexBufferCreated(ID3D11Buffer* vb, const void* initialData);

    // Called from the D3DCompile hook for each successful main_fs / mapfeature_fs
    // compile. Captures BLEND1/2/3 #define values (channel indices for blendMap,
    // 0..3) keyed by bytecode hash. OnPixelShaderCreated then moves the entry
    // into a PS*-keyed map for per-draw lookup.
    void OnTerrainPsCompiled(const void* bytecode, size_t size,
                             int blend1, int blend2, int blend3);
}
