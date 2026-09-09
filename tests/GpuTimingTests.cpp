#include "../src/GpuTiming.h"
#include <cassert>
#include <cstring>
#include <cstdint>

struct FakeContext
{
    HRESULT ready = S_FALSE;
    bool disjoint = false;
    UINT64 frequency = 1000, start = 10, finish = 12;
    int begins = 0, ends = 0, reads = 0;
    void Begin(ID3D11Query*) { ++begins; }
    void End(ID3D11Query*) { ++ends; }
    HRESULT GetData(ID3D11Query* query, void* out, UINT bytes, UINT flags)
    {
        assert(flags == D3D11_ASYNC_GETDATA_DONOTFLUSH);
        ++reads;
        if (ready != S_OK) return ready;
        if (bytes == sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT))
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT value = { frequency, disjoint };
            memcpy(out, &value, bytes);
        }
        else
        {
            const UINT64 value = (reinterpret_cast<uintptr_t>(query) % 3 == 2) ? start : finish;
            memcpy(out, &value, bytes);
        }
        return S_OK;
    }
};

int main()
{
    GpuTimingPhase timer;
    for (int i = 0; i < 2; ++i)
    {
        timer.disjoint[i] = reinterpret_cast<ID3D11Query*>(uintptr_t(1 + 3*i));
        timer.begin[i] = reinterpret_cast<ID3D11Query*>(uintptr_t(2 + 3*i));
        timer.end[i] = reinterpret_cast<ID3D11Query*>(uintptr_t(3 + 3*i));
    }
    FakeContext ctx;
    float ms = 7;
    assert(!timer.Collect(&ctx, ms) && ctx.reads == 0); // never poll unissued queries
    for (int i = 0; i < 2; ++i) { timer.Begin(&ctx); timer.End(&ctx); }
    for (int i = 0; i < 10; ++i)
    {
        assert(!timer.Collect(&ctx, ms));
        timer.Begin(&ctx); timer.End(&ctx);
    }
    assert(ctx.begins == 2 && ctx.ends == 6 && ms == 7); // pending slots were not reused
    ctx.ready = S_OK;
    assert(timer.Collect(&ctx, ms) && ms == 2);
    assert(!timer.pending[0] && !timer.pending[1]);
    assert(!timer.Collect(&ctx, ms)); // consume once
    for (int invalid = 0; invalid < 4; ++invalid)
    {
        timer.Begin(&ctx); timer.End(&ctx);
        ctx.disjoint = invalid == 0;
        ctx.frequency = invalid == 1 ? 0 : 1000;
        ctx.finish = invalid == 2 ? 9 : 12;
        ctx.ready = invalid == 3 ? E_FAIL : S_OK;
        assert(!timer.Collect(&ctx, ms) && ms == 2);
    }
    assert(ctx.begins == 6); // invalid samples do not permanently exhaust the ring
}
