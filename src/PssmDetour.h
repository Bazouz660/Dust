#pragma once

namespace PssmDetour
{

// Hooks Ogre::GpuSharedParameters::getFloatPointer (OgreMain_x64.dll export)
// to capture Kenshi's CSM cascade-splits source and allow live PSSM lambda
// overrides. Call TryInstall early in startPlugin — the capture fires when
// Kenshi's shadow orchestrator writes csmParams at scene init.
bool TryInstall();
bool IsInstalled();

// Set the PSSM lambda used to recompute Kenshi's cascade splits. 0.0 = pure
// linear (cascade 0 huge, far cascade tiny); 1.0 = pure logarithmic (cascade 0
// tiny, far cascade huge). Pass a NEGATIVE value to clear the override and
// restore Kenshi's own splits (they do not fit the PSSM formula at any single
// lambda, so "no override" is the only exact vanilla). Clamped to [0, 1].
// CSM shadow mode only — RTWSM has no cascades. Effect appears next frame.
void  SetLambda(float lambda);
float GetLambda();

// Live shadow far-distance override. Writes the CSM splits-source far
// (field_0x10[4], drives both the cbuffer split distances and the shadow
// camera frusta), the RTWSM shared-params shadow_range float, and intercepts
// the OGRE far-distance getters. A value matching the captured native far
// clears the override. Returns false if the CSM splits source hasn't been
// captured yet (the RTWSM path may still have applied).
bool  SetShadowFar(float farDistance);
float GetShadowFar();

}
