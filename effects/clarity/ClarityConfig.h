#pragma once

// ClarityConfig — plain data struct for Clarity / Local Contrast parameters.
// Values are populated by the framework via DustSettingDesc array.

struct ClarityConfig
{
    // Toggle
    bool enabled = true;

    // Clarity
    float strength        = 0.4f;   // Detail enhancement strength (0 = off, 2 = very strong)
    float midtoneProtect  = 0.5f;   // How much to protect shadows/highlights (0 = uniform, 1 = midtones only)
    float blurRadius      = 8.0f;   // Gaussian blur radius in pixels

    // Debug
    bool debugView = false;
};

extern ClarityConfig gClarityConfig;
