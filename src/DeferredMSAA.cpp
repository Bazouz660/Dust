#include "DeferredMSAA.h"
#include "MSAARedirect.h"
#include "DustLog.h"

namespace DeferredMSAA
{

static ID3D11Device* sDevice = nullptr;
static float sEdgeThreshold = 0.02f;
static int sDebugMode = 0;

static ID3D11Buffer* sCB = nullptr;

struct alignas(16) MSAACBLayout {
    uint32_t sampleCount;
    float edgeThreshold;
    float debugMode;
    float pad;
};
static_assert(sizeof(MSAACBLayout) == 16, "CB must be 16 bytes");

static void ReleaseCB()
{
    if (sCB) { sCB->Release(); sCB = nullptr; }
}

static bool EnsureCB()
{
    if (!sDevice) return false;
    if (sCB) return true;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(MSAACBLayout);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(sDevice->CreateBuffer(&bd, nullptr, &sCB));
}

void SetDevice(ID3D11Device* device)
{
    if (sDevice != device)
        ReleaseCB();
    sDevice = device;
}

void BindForLighting(ID3D11DeviceContext* ctx)
{
    uint32_t sampleCount = MSAARedirect::GetSampleCount();
    if (sampleCount < 2 || !sDevice) return;
    if (!EnsureCB()) return;

    ID3D11ShaderResourceView* msaa0 = MSAARedirect::GetColorSRV(0);
    ID3D11ShaderResourceView* msaa1 = MSAARedirect::GetColorSRV(1);
    ID3D11ShaderResourceView* msaa2 = MSAARedirect::GetColorSRV(2);
    if (!msaa0 || !msaa1 || !msaa2) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(sCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        MSAACBLayout* cb = (MSAACBLayout*)mapped.pData;
        cb->sampleCount = sampleCount;
        cb->edgeThreshold = sEdgeThreshold;
        cb->debugMode = (float)sDebugMode;
        cb->pad = 0;
        ctx->Unmap(sCB, 0);
    }

    ID3D11ShaderResourceView* srvs[3] = { msaa0, msaa1, msaa2 };
    ctx->PSSetShaderResources(10, 3, srvs);
    ctx->PSSetConstantBuffers(3, 1, &sCB);

    static bool sFirstBind = true;
    if (sFirstBind)
    {
        Log("DeferredMSAA: bound MSAA SRVs at t10-t12, CB at b3 (%ux)", sampleCount);
        sFirstBind = false;
    }
}

void UnbindAfterLighting(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* nullSRVs[3] = {};
    ctx->PSSetShaderResources(10, 3, nullSRVs);
}

void SetEdgeThreshold(float threshold) { sEdgeThreshold = threshold; }
float GetEdgeThreshold() { return sEdgeThreshold; }
void SetDebugMode(int mode) { sDebugMode = mode; }
int GetDebugMode() { return sDebugMode; }
uint32_t GetSampleCount() { return MSAARedirect::GetSampleCount(); }

void Shutdown()
{
    ReleaseCB();
    sDevice = nullptr;
}

} // namespace DeferredMSAA
