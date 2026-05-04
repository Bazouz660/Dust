#pragma once

namespace ShadowProbe
{

// Logs all OGRE compositor node defs and identifies which are shadow nodes
// (CompositorShadowNodeDef). For each shadow node, logs per-cascade fields
// (technique, pssmLambda, numSplits, ...). Fires once total — call every frame
// from the swap-chain hook installer until it succeeds.
//
// Required first move on Path B (Step 4 of docs/shadow_csm_improvement_plan.md)
// because Kenshi creates its shadow node programmatically; the name isn't in
// any compositor script.
bool TryProbe();
bool IsProbed();

}
