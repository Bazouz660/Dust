#include "OgreSwapHook.h"
#include "DustLog.h"
#include "DustGUI.h"
#include "D3D11Hook.h"

#include <ogre/OgreRoot.h>
#include <ogre/OgreRenderSystem.h>
#include <ogre/OgreRenderWindow.h>

#include <windows.h>
#include <cstring>

namespace OgreSwapHook
{

// ==================== Internal state ====================

static void* sOriginalSwapBuffers = nullptr;
static Ogre::RenderWindow* sHookedWindow = nullptr;
static int   sHookedSlot = -1;
static bool  sInstalled = false;
static IDXGISwapChain* sCachedSwapChain = nullptr;

// ==================== PMF decode ====================

// Decode an MSVC x64 virtual-function PMF thunk to find the vtable slot.
// Patterns produced by MSVC (single-inheritance virtual member function ptr):
//
//   48 8B 01           mov rax, [rcx]
//   FF 20              jmp [rax]                ; slot 0
//   FF 60 disp8        jmp [rax + disp8]        ; slot = disp8 / 8
//   FF A0 disp32       jmp [rax + disp32]       ; slot = disp32 / 8
//
// Returns -1 if the byte pattern doesn't match.
static int DecodeVirtualSlot(const void* pmfThunk)
{
    const unsigned char* p = (const unsigned char*)pmfThunk;
    if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x01) return -1;
    p += 3;
    if (p[0] != 0xFF) return -1;
    if (p[1] == 0x20) return 0;
    if (p[1] == 0x60) return p[2] / 8;
    if (p[1] == 0xA0) return *(const int*)(p + 2) / 8;
    return -1;
}

// Compile-time member-function-pointer to runtime vtable slot for
// Ogre::RenderTarget::swapBuffers. Robust to OGRE header drift since we
// derive the slot from MSVC's actual codegen for the PMF, not from a
// hand-counted slot index.
//
// MSVC x64 may emit a PMF of 8/16/24 bytes depending on inheritance visibility
// at the call site. In all cases the first 8 bytes are the function (or
// virtual-thunk) pointer; later words are this-adjustor / vbtable offsets
// that we don't care about since RenderTarget uses single inheritance.
static int FindSwapBuffersSlot()
{
    typedef void (Ogre::RenderTarget::*SwapPMF)();
    SwapPMF pmf = &Ogre::RenderTarget::swapBuffers;

    void* thunk = nullptr;
    std::memcpy(&thunk, &pmf, sizeof(thunk));
    return DecodeVirtualSlot(thunk);
}

// ==================== Hook ====================

// MSVC x64 calling convention for non-static member functions: `this` is in
// RCX, exactly like __thiscall on x64. So a free function with the same arg
// list works as the detour.
static void HookedSwapBuffers(Ogre::RenderTarget* self)
{
    // Render the GUI on top of the back buffer before the original commits
    // it to the screen.
    if (!D3D11Hook::IsShutdownSignaled())
        DustGUI::Render();

    typedef void (*SwapFn)(Ogre::RenderTarget*);
    ((SwapFn)sOriginalSwapBuffers)(self);
}

// ==================== Install ====================

bool TryInstall()
{
    if (sInstalled) return true;

    Ogre::Root* root = Ogre::Root::getSingletonPtr();
    if (!root)
    {
        Log("OgreSwapHook: Root singleton not yet available");
        return false;
    }

    Ogre::RenderWindow* rw = root->getAutoCreatedWindow();
    if (!rw)
    {
        Log("OgreSwapHook: getAutoCreatedWindow returned null");
        return false;
    }

    int slot = FindSwapBuffersSlot();
    if (slot < 0)
    {
        Log("OgreSwapHook: failed to decode swapBuffers PMF thunk (unknown MSVC layout)");
        return false;
    }

    void** vtable = *(void***)rw;
    sOriginalSwapBuffers = vtable[slot];

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        Log("OgreSwapHook: VirtualProtect failed (err=%lu)", GetLastError());
        sOriginalSwapBuffers = nullptr;
        return false;
    }
    vtable[slot] = (void*)HookedSwapBuffers;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &oldProtect);

    sHookedWindow = rw;
    sHookedSlot = slot;
    sInstalled = true;

    Log("OgreSwapHook: vtable-patched swapBuffers slot %d on RenderWindow %p (orig=%p)",
        slot, rw, sOriginalSwapBuffers);
    return true;
}

bool IsInstalled()
{
    return sInstalled;
}

}
