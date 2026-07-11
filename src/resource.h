// Resource IDs for binary assets embedded into Dust.dll at build time.
// The PNGs live in assets/ and are listed in Dust.rc — the resource compiler
// bakes them into the DLL, so no generated headers are needed. Load them with
// DustGUI's LoadTextureFromResource().
#pragma once

#define IDR_DISCORD_LOGO_PNG 101
#define IDR_GITHUB_LOGO_PNG  102
