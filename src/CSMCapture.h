#pragma once

// CSMCapture: snapshots Kenshi's deferred lighting CSM cbuffer fields each
// frame and exposes them through the host API for plugins (e.g. volumetric
// fog shadow sampling).
//
// Field offsets are discovered at shader-compile time via D3DReflect on the
// patched deferred main_fs (CSM variant, 608-byte $Params cbuffer). The
// snapshot is updated from D3D11Hook's HookedUnmap path when the engine
// commits the cbuffer for the next frame's deferred draw.

#include <d3d11.h>
#include <d3d11shader.h>

struct DustCSMData;  // defined in DustAPI.h

namespace CSMCapture
{

// Walk the cbuffer variables in the patched deferred PS and capture byte
// offsets for sunDirection / shadowViewMat / csmScale / csmTrans / csmParams.
// Safe to call on any reflector — only acts when the cbuffer matches the CSM
// 608-byte layout. Logs the result on first match.
void DiscoverOffsets(ID3D11ShaderReflection* reflector);

// Called from HookedUnmap on a tracked 608-byte cbuffer commit. Reads the
// fields at the discovered offsets into the snapshot. No-op if offsets
// haven't been discovered yet.
void OnUnmap(const void* cbufferData, unsigned int byteSize);

// Host API getter. Fills outData (caller-provided) and returns 1 if a
// complete snapshot is available, 0 otherwise.
int GetSnapshot(DustCSMData* outData);

}
