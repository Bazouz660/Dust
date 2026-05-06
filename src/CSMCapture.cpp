#include "CSMCapture.h"
#include "DustAPI.h"
#include "DustLog.h"

#include <atomic>
#include <mutex>
#include <cstring>

namespace CSMCapture
{

// === Discovered offsets ===
// Set at shader-compile time once we see the CSM-variant deferred PS. -1 = not
// yet discovered. csmScale/csmTrans/csmParams are float4[4] arrays (16 bytes
// per cascade); the offset is the byte offset of element [0]. shadowViewMat
// is a float4x4 (64 bytes). sunDirection is a float3 (or float4 padded to 16).
struct Offsets
{
    int sunDirection  = -1;
    int shadowViewMat = -1;
    int csmScale      = -1;
    int csmTrans      = -1;
    int csmParams     = -1;
    bool complete() const
    {
        return sunDirection >= 0 && shadowViewMat >= 0 &&
               csmScale >= 0 && csmTrans >= 0 && csmParams >= 0;
    }
};
static Offsets sOffsets;
static std::atomic<bool> sOffsetsReady{false};
static std::atomic<bool> sLogged{false};

// === Snapshot storage (double-buffered: writer locks, reader copies out) ===
static DustCSMData sSnapshot = {};
static std::mutex  sSnapshotMutex;
static std::atomic<bool> sFirstSnapshotLogged{false};

void DiscoverOffsets(ID3D11ShaderReflection* reflector)
{
    if (!reflector) return;
    if (sOffsetsReady.load()) return;

    D3D11_SHADER_DESC desc = {};
    if (FAILED(reflector->GetDesc(&desc))) return;

    for (UINT i = 0; i < desc.ConstantBuffers; i++)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = reflector->GetConstantBufferByIndex(i);
        if (!cb) continue;
        D3D11_SHADER_BUFFER_DESC cbDesc = {};
        if (FAILED(cb->GetDesc(&cbDesc))) continue;

        // Only the CSM variant of the deferred main_fs has the 608-byte $Params
        // cbuffer with all the cascade arrays. Other shaders (RTW variant,
        // light volumes, etc.) won't match.
        if (cbDesc.Size != 608) continue;

        Offsets candidate;
        for (UINT j = 0; j < cbDesc.Variables; j++)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
            if (!var) continue;
            D3D11_SHADER_VARIABLE_DESC vDesc = {};
            if (FAILED(var->GetDesc(&vDesc))) continue;
            const char* nm = vDesc.Name ? vDesc.Name : "";

            if (strcmp(nm, "sunDirection") == 0)       candidate.sunDirection  = (int)vDesc.StartOffset;
            else if (strcmp(nm, "shadowViewMat") == 0) candidate.shadowViewMat = (int)vDesc.StartOffset;
            else if (strcmp(nm, "csmScale") == 0)      candidate.csmScale      = (int)vDesc.StartOffset;
            else if (strcmp(nm, "csmTrans") == 0)      candidate.csmTrans      = (int)vDesc.StartOffset;
            else if (strcmp(nm, "csmParams") == 0)     candidate.csmParams     = (int)vDesc.StartOffset;
        }

        if (candidate.complete())
        {
            sOffsets = candidate;
            sOffsetsReady.store(true);
            if (!sLogged.exchange(true))
            {
                Log("CSMCapture: discovered offsets sunDir=%d shadowViewMat=%d "
                    "csmScale=%d csmTrans=%d csmParams=%d",
                    candidate.sunDirection, candidate.shadowViewMat,
                    candidate.csmScale, candidate.csmTrans, candidate.csmParams);
            }
            return;
        }
    }
}

void OnUnmap(const void* cbufferData, unsigned int byteSize)
{
    if (!cbufferData) return;
    if (byteSize != 608) return;
    if (!sOffsetsReady.load()) return;

    const char* base = (const char*)cbufferData;

    DustCSMData s;
    s.valid = 1;

    // sunDirection: float3 in HLSL, 16-byte aligned in cbuffer. Read 3 floats.
    const float* sd = (const float*)(base + sOffsets.sunDirection);
    s.sunDirection[0] = sd[0];
    s.sunDirection[1] = sd[1];
    s.sunDirection[2] = sd[2];

    // shadowViewMat: float4x4 = 16 floats.
    memcpy(s.shadowViewMat, base + sOffsets.shadowViewMat, 16 * sizeof(float));

    // csmScale / csmTrans / csmParams: each is float4[4] = 16 floats.
    memcpy(s.csmScale,  base + sOffsets.csmScale,  16 * sizeof(float));
    memcpy(s.csmTrans,  base + sOffsets.csmTrans,  16 * sizeof(float));
    memcpy(s.csmParams, base + sOffsets.csmParams, 16 * sizeof(float));

    {
        std::lock_guard<std::mutex> lock(sSnapshotMutex);
        sSnapshot = s;
    }

    // One-shot sanity dump.
    if (!sFirstSnapshotLogged.exchange(true))
    {
        Log("CSMCapture: first snapshot — sunDir=(%.4f, %.4f, %.4f)",
            (double)s.sunDirection[0], (double)s.sunDirection[1], (double)s.sunDirection[2]);
        // Full 4x4 matrix dump. Position of the (0,0,0,1) last row tells us
        // the storage convention:
        //   indices [3]=0,[7]=0,[11]=0,[15]=1 -> column_major
        //   indices [12]=0,[13]=0,[14]=0,[15]=1 -> row_major
        Log("CSMCapture: shadowViewMat[0..3]   = (%.4f, %.4f, %.4f, %.4f)",
            (double)s.shadowViewMat[0],  (double)s.shadowViewMat[1],
            (double)s.shadowViewMat[2],  (double)s.shadowViewMat[3]);
        Log("CSMCapture: shadowViewMat[4..7]   = (%.4f, %.4f, %.4f, %.4f)",
            (double)s.shadowViewMat[4],  (double)s.shadowViewMat[5],
            (double)s.shadowViewMat[6],  (double)s.shadowViewMat[7]);
        Log("CSMCapture: shadowViewMat[8..11]  = (%.4f, %.4f, %.4f, %.4f)",
            (double)s.shadowViewMat[8],  (double)s.shadowViewMat[9],
            (double)s.shadowViewMat[10], (double)s.shadowViewMat[11]);
        Log("CSMCapture: shadowViewMat[12..15] = (%.4f, %.4f, %.4f, %.4f)",
            (double)s.shadowViewMat[12], (double)s.shadowViewMat[13],
            (double)s.shadowViewMat[14], (double)s.shadowViewMat[15]);
        for (int i = 0; i < 4; i++)
        {
            const float* p = s.csmParams + i*4;
            const float* sc = s.csmScale + i*4;
            const float* tr = s.csmTrans + i*4;
            Log("CSMCapture: cascade %d split=%.3f scale=(%.5f,%.5f,%.5f) trans=(%.4f,%.4f,%.4f)",
                i, (double)p[0],
                (double)sc[0], (double)sc[1], (double)sc[2],
                (double)tr[0], (double)tr[1], (double)tr[2]);
        }
    }
}

int GetSnapshot(DustCSMData* outData)
{
    if (!outData) return 0;
    std::lock_guard<std::mutex> lock(sSnapshotMutex);
    if (!sSnapshot.valid) return 0;
    *outData = sSnapshot;
    return 1;
}

}
