#include "ShadowProbe.h"
#include "DustLog.h"

#include <ogre/OgreRoot.h>
#include <ogre/Compositor/OgreCompositorManager2.h>

#include <windows.h>

namespace ShadowProbe
{

static bool sProbed = false;

bool IsProbed() { return sProbed; }

bool TryProbe()
{
    if (sProbed) return true;

    Ogre::Root* root = Ogre::Root::getSingletonPtr();
    if (!root)
    {
        // Throttle: log the wait state once per second of frames (~60).
        static int sWaitCounter = 0;
        if ((sWaitCounter++ % 60) == 0)
            Log("ShadowProbe: waiting for Ogre::Root (call #%d)", sWaitCounter);
        return false;
    }

    // Dual lookup: header-inline (reads mCompositorManager2 at our header's
    // offset) vs. DLL-exported (the real getter). If they disagree, we have a
    // header/binary layout mismatch and need to trust the exported version.
    Ogre::CompositorManager2* cmInline = root->getCompositorManager2();

    typedef Ogre::CompositorManager2* (*GetCM2Fn)(const Ogre::Root*);
    static GetCM2Fn sGetCM2Exported = nullptr;
    if (!sGetCM2Exported)
    {
        HMODULE ogreMain = GetModuleHandleA("OgreMain_x64.dll");
        if (ogreMain)
            sGetCM2Exported = (GetCM2Fn)GetProcAddress(ogreMain,
                "?getCompositorManager2@Root@Ogre@@QEBAPEAVCompositorManager2@2@XZ");
    }
    Ogre::CompositorManager2* cmExported = sGetCM2Exported ? sGetCM2Exported(root) : nullptr;

    Ogre::CompositorManager2* cm = cmExported ? cmExported : cmInline;

    if (!cm)
    {
        static int sWaitCounter = 0;
        if ((sWaitCounter++ % 60) == 0)
            Log("ShadowProbe: waiting for CompositorManager2 (root=%p, "
                "cmInline=%p, cmExported=%p, getCM2Sym=%p, call #%d)",
                root, cmInline, cmExported, sGetCM2Exported, sWaitCounter);
        return false;
    }

    if (cmInline != cmExported)
    {
        Log("ShadowProbe: WARNING — header/binary layout mismatch on Root. "
            "cmInline=%p (header offset), cmExported=%p (DLL truth). Using exported.",
            cmInline, cmExported);
    }

    // STOP HERE: header/binary layout drift means *any* field read on
    // CompositorManager2 (including iterating mNodeDefinitions via the
    // inline getNodeDefinitions accessor) reads garbage. The previous
    // attempt iterated bogus map memory and hung the game. Until we have
    // a layout-independent enumeration path (calling only exported member
    // functions, not inline accessors that touch private fields), the
    // probe stops at confirming the OGRE singletons are alive.
    Log("ShadowProbe: Root=%p CompositorManager2=%p — singletons alive. "
        "Enumeration disabled pending a layout-safe approach.",
        root, cm);
    sProbed = true;
    return true;
}

}
