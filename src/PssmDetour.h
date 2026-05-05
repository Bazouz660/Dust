#pragma once

namespace PssmDetour
{

bool TryInstall();
bool IsInstalled();

// Set the PSSM lambda used to recompute Kenshi's cascade splits. 0.0 = pure
// linear (cascade 0 huge, far cascade tiny); 1.0 = pure logarithmic (cascade 0
// tiny, far cascade huge). Kenshi's native value back-solves to ~0.95-0.97.
// Clamped to [0, 1]. Effect appears next frame.
void  SetLambda(float lambda);
float GetLambda();

// Per-cascade filter-radius multiplier on top of Kenshi's vanilla per-cascade
// filter taper (csmParams[i].y). idx in [0..3]; scale clamped to [0, 5]. The
// global "Filter Radius" slider remains in effect on top of this.
void  SetCascadeFilterScale(int cascadeIdx, float scale);
float GetCascadeFilterScale(int cascadeIdx);

// Apply the per-cascade filter scales in-place to a 608-byte CSM-mode
// $Params cbuffer. Multiplies csmParams[i].y at offsets 212/228/244/260
// by sCascadeFilterScale[i]. Call from the cbuffer-commit hook so the
// scale lands in whatever values Kenshi wrote (direct cbuffer write,
// auto-constant, per-pass param — whichever).
void  ApplyFilterScalesToCbuffer(void* cbufferData);

}
