#pragma once

// RtScene — feeds the DXR sidecar with the rasterized scene.
//
// Harvest() (POST_LIGHTING, capture pointers valid) walks GeometryReplay's
// per-frame cache: static-category draws whose mesh range is known become TLAS
// instances immediately; unseen ranges trigger a one-time staging readback of
// their VB/IB pair (mapped stall-free on a later frame), are parsed into two
// big GPU pools (float3 positions / u32 indices), and get a BLAS built from
// pool VAs. PRE-TRANSFORMED draws (verts already world-space) use an identity
// instance transform; WORLD-PLACED draws use the replay's recovered world
// matrix. Skinned/foliage/terrain casters are later phases.

#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>

namespace RtScene
{
    void Init();
    void Shutdown();
    void Flush();    // drop every BLAS/pool (GUI button; safe, rebuilds over the next frames)

    // Called from the device CreateInputLayout hook; records where POSITION
    // lives (offset/format/slot) so VB parsing doesn't have to guess.
    void OnInputLayoutCreated(const D3D11_INPUT_ELEMENT_DESC* descs, UINT count,
                              ID3D11InputLayout* layout);

    // POST_LIGHTING: schedule readbacks, parse completed ones, snapshot this
    // frame's instance list. No CPU/GPU sync stalls.
    void Harvest(ID3D11DeviceContext* ctx, ID3D11Device* device);

    // During D3D12 recording: upload parsed meshes, build pending BLASes,
    // rebuild the TLAS. frameSlot selects the per-frame instance-info buffer
    // (0..2). Returns true when a non-empty TLAS is ready to trace.
    bool RecordBuild(ID3D12GraphicsCommandList4* cmd, uint32_t frameSlot);

    // Writes pos_pool.bin / idx_pool.bin / instances.bin / meshes.txt into dir
    // (one-time GPU readback + CPU wait). Part of the offline replay snapshot.
    bool DumpGeometry(const char* dir);

    ID3D12Resource* Tlas();
    ID3D12Resource* PosPool();
    ID3D12Resource* IdxPool();
    ID3D12Resource* InstInfoBuffer(uint32_t frameSlot);
    uint32_t InstanceCount();

    struct Stats
    {
        uint32_t meshes = 0;
        uint32_t blasBuilt = 0;
        uint32_t instances = 0;
        uint32_t pendingPairs = 0;
        uint32_t skippedInstanced = 0;
        uint32_t parseFailures = 0;
        uint32_t layoutsTracked = 0;
        uint64_t poolVertsUsed = 0;
        uint64_t poolIdxUsed = 0;
    };
    Stats GetStats();
}
