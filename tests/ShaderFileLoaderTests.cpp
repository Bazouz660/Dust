#include <windows.h>
#include <d3dcompiler.h>
#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <filesystem>
static void Log(const char* format, ...) {
    va_list args; va_start(args,format); std::vfprintf(stderr,format,args); va_end(args);
}
#include "build/ShaderFileLoader.generated.h"
int main() {
    // The production host previously passed a null include handler. Testing
    // D3DCompileFromFile alone missed the deployed Outline loading failure.
    for (const char* file : {"outline_ps.hlsl","outline_debug_ps.hlsl"}) {
        const auto path = std::filesystem::absolute(std::filesystem::path("../effects/outline/shaders")/file).string();
        auto* shader = HostCompileShaderFromFile(path.c_str(),"main","ps_5_0");
        assert(shader); shader->Release();
    }
    std::puts("Production file loader resolves Outline includes outside the working directory");
}
