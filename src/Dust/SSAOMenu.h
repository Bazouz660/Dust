#pragma once

namespace SSAOMenu
{
    // Poll the settings.cfg "Dust_SSAO" toggle state.
    // Returns true if the node exists and is enabled, false otherwise.
    // Falls back to gSSAOConfig.enabled if compositor not found.
    bool PollCompositorEnabled();
}
