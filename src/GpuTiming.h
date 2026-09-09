#pragma once
#include <d3d11.h>

// Explicit query ownership: a slot is reusable only after its sample is consumed.
// Release is explicit because LoadedEffect entries are moved/sorted by the loader.
struct GpuTimingPhase
{
    ID3D11Query* disjoint[2] = {};
    ID3D11Query* begin[2] = {};
    ID3D11Query* end[2] = {};
    bool pending[2] = {};
    int next = 0;
    int recording = -1;

    void Release()
    {
        for (int i = 0; i < 2; ++i)
        {
            if (disjoint[i]) disjoint[i]->Release();
            if (begin[i]) begin[i]->Release();
            if (end[i]) end[i]->Release();
        }
        *this = GpuTimingPhase();
    }

    bool Init(ID3D11Device* device)
    {
        Release();
        for (int i = 0; i < 2; ++i)
        {
            D3D11_QUERY_DESC desc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            HRESULT hr = device->CreateQuery(&desc, &disjoint[i]);
            desc.Query = D3D11_QUERY_TIMESTAMP;
            if (SUCCEEDED(hr)) hr = device->CreateQuery(&desc, &begin[i]);
            if (SUCCEEDED(hr)) hr = device->CreateQuery(&desc, &end[i]);
            if (FAILED(hr)) { Release(); return false; }
        }
        return true;
    }

    template<class Context> bool Collect(Context* ctx, float& milliseconds)
    {
        bool collected = false;
        // next is the oldest slot when both are occupied. Consume in order.
        for (int n = 0; n < 2; ++n)
        {
            const int i = (next + n) % 2;
            if (!pending[i]) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data = {};
            UINT64 start = 0, finish = 0;
            HRESULT hr = ctx->GetData(disjoint[i], &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) break;
            if (hr == S_OK)
            {
                hr = ctx->GetData(begin[i], &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr == S_FALSE) break;
                if (hr == S_OK) hr = ctx->GetData(end[i], &finish, sizeof(finish), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr == S_FALSE) break;
            }
            pending[i] = false; // completed (possibly invalid) or failed; never publish garbage
            if (hr == S_OK && !data.Disjoint && data.Frequency && finish >= start)
            {
                milliseconds = float(double(finish - start) * 1000.0 / double(data.Frequency));
                collected = true;
            }
        }
        return collected;
    }

    template<class Context> void Begin(Context* ctx)
    {
        if (recording >= 0) return;
        const int i = next;
        if (pending[i] || !disjoint[i] || !begin[i] || !end[i]) return;
        ctx->Begin(disjoint[i]);
        ctx->End(begin[i]);
        recording = i;
    }

    template<class Context> void End(Context* ctx)
    {
        if (recording < 0) return; // Begin skipped because the GPU is behind
        const int i = recording;
        ctx->End(end[i]);
        ctx->End(disjoint[i]);
        pending[i] = true;
        next = 1 - i;
        recording = -1;
    }
};
