#include "GeometryReplay.h"
#include "GeometryCapture.h"
#include "DustLog.h"
#include <cstring>

namespace GeometryReplay
{

static uint32_t sReplaysIssued = 0;

static void MatMul4x4(float* out, const float* a, const float* b)
{
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            out[r * 4 + c] =
                a[r * 4 + 0] * b[0 * 4 + c] +
                a[r * 4 + 1] * b[1 * 4 + c] +
                a[r * 4 + 2] * b[2 * 4 + c] +
                a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

struct ScratchCBEntry
{
    ID3D11Buffer* buffer;
    uint32_t      size;
};
static std::vector<ScratchCBEntry> sScratchCBs;

static ID3D11Buffer* GetScratchCB(ID3D11Device* device, uint32_t requiredSize)
{
    for (auto& entry : sScratchCBs)
    {
        if (entry.size == requiredSize)
            return entry.buffer;
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = requiredSize;
    desc.Usage           = D3D11_USAGE_DEFAULT;
    desc.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;

    ID3D11Buffer* buf = nullptr;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &buf);
    if (FAILED(hr) || !buf)
        return nullptr;

    sScratchCBs.push_back({ buf, requiredSize });
    return buf;
}

// Save/restore IA + VS state around the replay batch.
// Lighter than D3D11StateBlock — only touches what replay modifies.
struct ReplayStateBlock
{
    ID3D11InputLayout*       iaLayout   = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY iaTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer*            vbs[CapturedDraw::MAX_VB_SLOTS] = {};
    UINT                     vbStrides[CapturedDraw::MAX_VB_SLOTS] = {};
    UINT                     vbOffsets[CapturedDraw::MAX_VB_SLOTS] = {};
    ID3D11Buffer*            ib         = nullptr;
    DXGI_FORMAT              ibFormat   = DXGI_FORMAT_R16_UINT;
    UINT                     ibOffset   = 0;
    ID3D11VertexShader*      vs         = nullptr;
    ID3D11Buffer*            vsCBs[CapturedDraw::MAX_VS_CBS] = {};

    void Capture(ID3D11DeviceContext* ctx)
    {
        ctx->IAGetInputLayout(&iaLayout);
        ctx->IAGetPrimitiveTopology(&iaTopology);
        ctx->IAGetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs, vbStrides, vbOffsets);
        ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, CapturedDraw::MAX_VS_CBS, vsCBs);
    }

    void Restore(ID3D11DeviceContext* ctx)
    {
        ctx->IASetInputLayout(iaLayout);
        ctx->IASetPrimitiveTopology(iaTopology);
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs, vbStrides, vbOffsets);
        ctx->IASetIndexBuffer(ib, ibFormat, ibOffset);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, CapturedDraw::MAX_VS_CBS, vsCBs);
        Release();
    }

    void Release()
    {
        if (iaLayout) { iaLayout->Release(); iaLayout = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VB_SLOTS; i++)
            if (vbs[i]) { vbs[i]->Release(); vbs[i] = nullptr; }
        if (ib) { ib->Release(); ib = nullptr; }
        if (vs) { vs->Release(); vs = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VS_CBS; i++)
            if (vsCBs[i]) { vsCBs[i]->Release(); vsCBs[i] = nullptr; }
    }
};

uint32_t Replay(ID3D11DeviceContext* ctx, ID3D11Device* device,
                const float* replacementVP)
{
    const auto& captures = GeometryCapture::GetCaptures();
    if (captures.empty())
        return 0;

    ReplayStateBlock saved;
    saved.Capture(ctx);

    uint32_t replayed = 0;
    static std::vector<uint8_t> cbDataBuf;

    for (const auto& draw : captures)
    {
        if (!draw.vsMetadata || draw.vsMetadata->transformType == VSTransformType::UNKNOWN)
            continue;
        if (!draw.cbStagingCopy)
            continue;

        const VSConstantBufferInfo& meta = *draw.vsMetadata;

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ctx->Map(draw.cbStagingCopy, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
            continue;

        if (cbDataBuf.size() < draw.cbStagingSize)
            cbDataBuf.resize(draw.cbStagingSize);
        memcpy(cbDataBuf.data(), mapped.pData, draw.cbStagingSize);
        ctx->Unmap(draw.cbStagingCopy, 0);

        if (meta.clipMatrixOffset + 64 > draw.cbStagingSize)
            continue;

        float* clipDst = reinterpret_cast<float*>(cbDataBuf.data() + meta.clipMatrixOffset);

        bool useWorldFromCB = (meta.transformType == VSTransformType::STATIC &&
                               draw.instanceCount <= 1);

        if (useWorldFromCB)
        {
            if (meta.worldMatrixOffset + 64 <= draw.cbStagingSize &&
                meta.worldMatrixSize >= 64)
            {
                const float* world = reinterpret_cast<const float*>(
                    cbDataBuf.data() + meta.worldMatrixOffset);
                MatMul4x4(clipDst, world, replacementVP);
            }
            else
            {
                memcpy(clipDst, replacementVP, 64);
            }
        }
        else
        {
            memcpy(clipDst, replacementVP, 64);
        }

        ID3D11Buffer* scratchCB = GetScratchCB(device, draw.cbStagingSize);
        if (!scratchCB)
            continue;

        ctx->UpdateSubresource(scratchCB, 0, nullptr, cbDataBuf.data(), 0, 0);

        // Set IA state (both VB slots — slot 1 has instance data for instanced draws)
        ctx->IASetInputLayout(draw.inputLayout);
        ctx->IASetPrimitiveTopology(draw.topology);
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, draw.vertexBuffers,
                                draw.vbStrides, draw.vbOffsets);
        ctx->IASetIndexBuffer(draw.indexBuffer, draw.indexFormat, draw.ibOffset);

        // Set VS and constant buffers
        ctx->VSSetShader(draw.vs, nullptr, 0);
        for (UINT i = 0; i < CapturedDraw::MAX_VS_CBS; i++)
        {
            if (i == meta.cbSlot)
                ctx->VSSetConstantBuffers(i, 1, &scratchCB);
            else if (draw.vsCBs[i])
                ctx->VSSetConstantBuffers(i, 1, &draw.vsCBs[i]);
        }

        if (draw.instanceCount > 1)
        {
            ctx->DrawIndexedInstanced(draw.indexCount, draw.instanceCount,
                                      draw.startIndexLocation, draw.baseVertexLocation,
                                      draw.startInstanceLocation);
        }
        else
        {
            ctx->DrawIndexed(draw.indexCount, draw.startIndexLocation,
                             draw.baseVertexLocation);
        }

        replayed++;
    }

    saved.Restore(ctx);

    if (sReplaysIssued < 3 && replayed > 0)
    {
        Log("GeometryReplay: replayed %u / %u draws", replayed, (uint32_t)captures.size());
        sReplaysIssued++;
    }

    return replayed;
}

uint32_t ReplayEx(ID3D11DeviceContext* ctx, ID3D11Device* device,
                  const float* replacementVP,
                  void (*preDrawCB)(ID3D11DeviceContext* ctx,
                                   uint32_t drawIndex,
                                   uint32_t priorTriangles,
                                   void* userdata),
                  void* userdata)
{
    const auto& captures = GeometryCapture::GetCaptures();
    if (captures.empty())
        return 0;

    ReplayStateBlock saved;
    saved.Capture(ctx);

    uint32_t replayed        = 0;
    uint32_t priorTriangles  = 0;
    static std::vector<uint8_t> cbDataBufEx;

    for (uint32_t i = 0; i < (uint32_t)captures.size(); i++)
    {
        const CapturedDraw& draw = captures[i];

        if (!draw.vsMetadata || draw.vsMetadata->transformType == VSTransformType::UNKNOWN)
            continue;
        if (!draw.cbStagingCopy)
            continue;

        const VSConstantBufferInfo& meta = *draw.vsMetadata;

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ctx->Map(draw.cbStagingCopy, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
            continue;

        if (cbDataBufEx.size() < draw.cbStagingSize)
            cbDataBufEx.resize(draw.cbStagingSize);
        memcpy(cbDataBufEx.data(), mapped.pData, draw.cbStagingSize);
        ctx->Unmap(draw.cbStagingCopy, 0);

        if (meta.clipMatrixOffset + 64 > draw.cbStagingSize)
            continue;

        float* clipDst = reinterpret_cast<float*>(cbDataBufEx.data() + meta.clipMatrixOffset);

        bool useWorldFromCB = (meta.transformType == VSTransformType::STATIC &&
                               draw.instanceCount <= 1);
        if (useWorldFromCB)
        {
            if (meta.worldMatrixOffset + 64 <= draw.cbStagingSize &&
                meta.worldMatrixSize >= 64)
            {
                const float* world = reinterpret_cast<const float*>(
                    cbDataBufEx.data() + meta.worldMatrixOffset);
                MatMul4x4(clipDst, world, replacementVP);
            }
            else
            {
                memcpy(clipDst, replacementVP, 64);
            }
        }
        else
        {
            memcpy(clipDst, replacementVP, 64);
        }

        ID3D11Buffer* scratchCB = GetScratchCB(device, draw.cbStagingSize);
        if (!scratchCB)
            continue;

        ctx->UpdateSubresource(scratchCB, 0, nullptr, cbDataBufEx.data(), 0, 0);

        ctx->IASetInputLayout(draw.inputLayout);
        ctx->IASetPrimitiveTopology(draw.topology);
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, draw.vertexBuffers,
                                draw.vbStrides, draw.vbOffsets);
        ctx->IASetIndexBuffer(draw.indexBuffer, draw.indexFormat, draw.ibOffset);

        ctx->VSSetShader(draw.vs, nullptr, 0);
        for (UINT j = 0; j < CapturedDraw::MAX_VS_CBS; j++)
        {
            if (j == meta.cbSlot)
                ctx->VSSetConstantBuffers(j, 1, &scratchCB);
            else if (draw.vsCBs[j])
                ctx->VSSetConstantBuffers(j, 1, &draw.vsCBs[j]);
        }

        // Fire the pre-draw callback AFTER VS state is set but BEFORE the draw call.
        if (preDrawCB)
            preDrawCB(ctx, i, priorTriangles, userdata);

        if (draw.instanceCount > 1)
        {
            ctx->DrawIndexedInstanced(draw.indexCount, draw.instanceCount,
                                      draw.startIndexLocation, draw.baseVertexLocation,
                                      draw.startInstanceLocation);
        }
        else
        {
            ctx->DrawIndexed(draw.indexCount, draw.startIndexLocation,
                             draw.baseVertexLocation);
        }

        priorTriangles += draw.indexCount / 3;
        replayed++;
    }

    saved.Restore(ctx);
    return replayed;
}

void Shutdown()
{
    for (auto& entry : sScratchCBs)
        if (entry.buffer) entry.buffer->Release();
    sScratchCBs.clear();
}

} // namespace GeometryReplay
