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

}
