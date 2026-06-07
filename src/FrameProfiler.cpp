#include "FrameProfiler.h"
#include "D3D11Hook.h"
#include "DustLog.h"
#include <cstdint>

namespace FrameProfiler
{
// Timestamp markers in frame execution order.
enum Marker {
    M_FRAME_BEGIN,
    M_GBUFFER_BEGIN, M_GBUFFER_END,
    M_SHADOW_BEGIN,  M_SHADOW_END,
    M_LIGHTING, M_FOG, M_TONEMAP,
    M_FRAME_END,
    M_COUNT
};

static bool          sEnabled = false;
static ID3D11Device* sDevice  = nullptr;

// Double-buffered: write one buffer this frame, read the other (last frame).
static ID3D11Query*  sDisjoint[2] = {};
static ID3D11Query*  sTs[2][M_COUNT] = {};
static uint32_t      sWritten[2] = { 0, 0 };  // which markers were ended this frame
static int           sWriteIdx = 0;
static bool          sFrameOpen = false;

static float         sSegMs[SEG_COUNT] = {};
static float         sFrameMs = 0.0f;

static void CreateQueries()
{
    if (!sDevice || sDisjoint[0]) return;
    D3D11_QUERY_DESC qd = {};
    for (int b = 0; b < 2; b++)
    {
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        sDevice->CreateQuery(&qd, &sDisjoint[b]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (int m = 0; m < M_COUNT; m++)
            sDevice->CreateQuery(&qd, &sTs[b][m]);
    }
}

static void DestroyQueries()
{
    for (int b = 0; b < 2; b++)
    {
        if (sDisjoint[b]) { sDisjoint[b]->Release(); sDisjoint[b] = nullptr; }
        for (int m = 0; m < M_COUNT; m++)
            if (sTs[b][m]) { sTs[b][m]->Release(); sTs[b][m] = nullptr; }
    }
}

void Init(ID3D11Device* device) { sDevice = device; }

void Shutdown()
{
    DestroyQueries();
    sDevice = nullptr;
    sEnabled = false;
    sFrameOpen = false;
}

bool IsEnabled() { return sEnabled; }

void SetEnabled(bool enabled)
{
    if (enabled == sEnabled) return;
    sEnabled = enabled;
    if (enabled) CreateQueries();
    sFrameOpen = false;
    for (int i = 0; i < SEG_COUNT; i++) sSegMs[i] = 0.0f;
    sFrameMs = 0.0f;
    // Pass-boundary marks live in the context hooks; (de)activate them.
    D3D11Hook::RefreshContextHooks();
}

static inline void Mark(ID3D11DeviceContext* ctx, Marker m)
{
    if (!sEnabled || !sFrameOpen || !sTs[sWriteIdx][m]) return;
    ctx->End(sTs[sWriteIdx][m]);
    sWritten[sWriteIdx] |= (1u << m);
}

void MarkGBufferBegin(ID3D11DeviceContext* ctx) { Mark(ctx, M_GBUFFER_BEGIN); }
void MarkGBufferEnd  (ID3D11DeviceContext* ctx) { Mark(ctx, M_GBUFFER_END);   }
void MarkShadowBegin (ID3D11DeviceContext* ctx) { Mark(ctx, M_SHADOW_BEGIN);  }
void MarkShadowEnd   (ID3D11DeviceContext* ctx) { Mark(ctx, M_SHADOW_END);    }
void MarkLighting    (ID3D11DeviceContext* ctx) { Mark(ctx, M_LIGHTING);      }
void MarkFog         (ID3D11DeviceContext* ctx) { Mark(ctx, M_FOG);           }
void MarkTonemap     (ID3D11DeviceContext* ctx) { Mark(ctx, M_TONEMAP);       }

static void ReadAndCompute(ID3D11DeviceContext* ctx, int b)
{
    if (!sDisjoint[b]) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    if (ctx->GetData(sDisjoint[b], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return;  // not ready yet (or this buffer never ran) — keep last results
    if (dj.Disjoint || dj.Frequency == 0)
        return;

    uint32_t mask = sWritten[b];
    UINT64 ts[M_COUNT] = {};
    for (int m = 0; m < M_COUNT; m++)
    {
        if (mask & (1u << m))
        {
            if (ctx->GetData(sTs[b][m], &ts[m], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                return;  // incomplete — try again next frame
        }
    }

    const double freq = (double)dj.Frequency;
    auto seg = [&](Marker a, Marker bb) -> float {
        if ((mask & (1u << a)) && (mask & (1u << bb)) && ts[bb] >= ts[a])
            return (float)((double)(ts[bb] - ts[a]) / freq * 1000.0);
        return 0.0f;
    };
    float raw[SEG_COUNT];
    raw[SEG_GBUFFER]  = seg(M_GBUFFER_BEGIN, M_GBUFFER_END);
    raw[SEG_SHADOW]   = seg(M_SHADOW_BEGIN,  M_SHADOW_END);
    raw[SEG_LIGHTING] = seg(M_LIGHTING,      M_FOG);
    raw[SEG_FOG]      = seg(M_FOG,           M_TONEMAP);
    raw[SEG_POST]     = seg(M_TONEMAP,       M_FRAME_END);

    // GPU busy time = sum of the measured pass spans. Do NOT use
    // FRAME_END - FRAME_BEGIN: that's the whole GPU timeline span including idle
    // (vsync/CPU waits), which ~= wall time and reads ~100% regardless of load.
    float busy = 0.0f;
    for (int i = 0; i < SEG_COUNT; i++) busy += raw[i];

    // Exponential smoothing — per-frame timings jitter and update ~160x/s, which
    // is unreadable. Smooth toward the new values so the readout is stable.
    const float a = 0.1f;
    for (int i = 0; i < SEG_COUNT; i++) sSegMs[i] += a * (raw[i] - sSegMs[i]);
    sFrameMs += a * (busy - sFrameMs);
}

void OnPresent(ID3D11DeviceContext* ctx)
{
    if (!sEnabled) { sFrameOpen = false; return; }
    CreateQueries();
    if (!sDisjoint[0]) return;

    // Close the frame we were writing.
    if (sFrameOpen)
    {
        ctx->End(sTs[sWriteIdx][M_FRAME_END]);
        sWritten[sWriteIdx] |= (1u << M_FRAME_END);
        ctx->End(sDisjoint[sWriteIdx]);
    }

    // Read the other buffer (previous frame; GPU has had a frame to finish).
    int prev = 1 - sWriteIdx;
    ReadAndCompute(ctx, prev);

    // Start the next frame in that buffer.
    sWriteIdx = prev;
    ctx->Begin(sDisjoint[sWriteIdx]);
    sWritten[sWriteIdx] = 0;
    sFrameOpen = true;
    ctx->End(sTs[sWriteIdx][M_FRAME_BEGIN]);
    sWritten[sWriteIdx] |= (1u << M_FRAME_BEGIN);
}

float GetSegmentMs(Segment s) { return (s >= 0 && s < SEG_COUNT) ? sSegMs[s] : 0.0f; }
float GetFrameMs() { return sFrameMs; }

const char* GetSegmentName(Segment s)
{
    switch (s)
    {
    case SEG_GBUFFER:  return "GBuffer";
    case SEG_SHADOW:   return "Shadow map";
    case SEG_LIGHTING: return "Lighting";
    case SEG_FOG:      return "Fog/Transp";
    case SEG_POST:     return "Post/UI";
    default:           return "?";
    }
}

} // namespace FrameProfiler
