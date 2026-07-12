#pragma once

// Reads the game's OGRE camera view-projection matrix each frame via KenshiLib's
// reconstructed structs (GameWorld -> player -> camera -> Ogre::Camera). Isolated in its
// own TU so the heavy kenshi/OGRE headers don't leak into the D3D11 framework code.
// The interface is plain floats — no OGRE types.

// Called from the game-loop hook with the GameWorld* it receives (as void*).
void CameraAccess_SetGameWorld(void* gameWorld);

// Fills outVP with the camera view-projection as 16 column-major floats (ready for a D3D
// constant buffer / HLSL default packing, same convention as the game's worldViewProjMatrix).
// Returns false if the camera isn't reachable yet (menu / loading) or an access faulted.
bool CameraAccess_GetViewProj(float outVP[16]);

// Camera near/far clip distances and vertical FOV (radians), extracted from the OGRE projection
// matrix (D3D RSDepth). Used by the upscaler (FSR2 needs them) and exposed to effects via the API.
// Returns false (and leaves outputs untouched) if the camera isn't reachable or values look invalid.
bool CameraAccess_GetCameraParams(float* nearZ, float* farZ, float* fovYRadians);

// Diagnostic: last step reached in the camera walk (1..8 = OK progression; a NEGATIVE value
// means an access violation was caught at that step — e.g. -5 = faulted calling getViewMatrix).
int CameraAccess_Status();
