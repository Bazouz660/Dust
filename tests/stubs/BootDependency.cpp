#include <cstdlib>
// DustBoot imports only these two KenshiLib functions. The disabled-DLL smoke
// test must never call either; use a shim so no real game code runs in the probe.
__declspec(dllexport) void DebugLog(const char*) { std::abort(); }
namespace KenshiLib {
enum HookStatus { SUCCESS, FAIL };
__declspec(dllexport) HookStatus AddHook(void*, void*, void**) { std::abort(); }
}
