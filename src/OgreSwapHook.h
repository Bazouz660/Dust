#pragma once

namespace OgreSwapHook
{
    // Try to vtable-patch Ogre::D3D11RenderWindow::swapBuffers so we render
    // ImGui inside OGRE's render path instead of the DXGI Present hook.
    //
    // The OGRE swap path is the game's own rendering boundary — only the real
    // game window goes through it, never any Havok loader / splash transient
    // swap chain. So routing GUI through here sidesteps the splash-Present
    // crash class entirely.
    //
    // Must be called after gGameAlive (so OGRE's Root and the primary render
    // window exist). Returns true if the patch was installed.
    bool TryInstall();

    // True if the OGRE swapBuffers patch is live. The DXGI Present hook
    // checks this and skips its GUI work to avoid double-render.
    bool IsInstalled();
}
