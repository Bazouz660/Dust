"""Build and run standalone MSVC probes (no Kenshi process or deployment)."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
msbuild = subprocess.check_output([
    str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.Component.MSBuild",
    "-find", "MSBuild/**/Bin/amd64/MSBuild.exe",
], text=True).strip().splitlines()[0]
# Some launchers provide both PATH and Path. MSBuild's environment map rejects that.
build_env = {key.upper(): value for key, value in os.environ.items()}
names = sys.argv[1:] or [p.stem for p in sorted(root.glob("*Tests.cpp"))]
if not names:
    raise SystemExit("No native tests found")
for name in names:
    if not name.isidentifier() or not (root / (name + ".cpp")).is_file():
        raise SystemExit("Unknown test: " + name)
    if name == "BootLoadOrderTests":
        source = (root.parent / "boot/DustBoot.cpp").read_text()
        begin = source.index("__declspec(dllexport) void startPlugin()")
        end = source.index("// ==================== DllMain", begin)
        output = root / "build/BootStartup.generated.h"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(source[begin:end])
    if name == "ShaderFileLoaderTests":
        source = (root.parent / "src/EffectLoader.cpp").read_text()
        begin = source.index("static ID3DBlob* HostCompileShaderFromFile(")
        end = source.index("static void HostDrawFullscreenTriangle(", begin)
        output = root / "build/ShaderFileLoader.generated.h"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(source[begin:end])
    if name == "ShadowAtlasTests":
        # Compile the actual shadow manager/hook bodies in isolation from the
        # upscaler, GUI and Kenshi hooks in the rest of D3D11Hook.cpp. The probe
        # supplies raw WARP calls as trampolines; no game process is touched.
        source = (root.parent / "src/D3D11Hook.cpp").read_text()
        spans = [
            ("// Runtime shadow atlas resize state.", "// ===================== Temporal sub-pixel jitter"),
            ("static bool IsShadowAtlasDesc(", "static HRESULT STDMETHODCALLTYPE HookedCreateSamplerState("),
            ("static int LookupShadowEntry(", "// ==================== Install ===================="),
        ]
        # The last span ends after the viewport hook, before unrelated Present code.
        last_start = source.index(spans[2][0])
        viewport_end = source.index("    oRSSetViewports(pThis, NumViewports, pViewports);", last_start)
        viewport_end = source.index("\n}", viewport_end) + 2
        slices = [source[source.index(a):source.index(b, source.index(a))] for a, b in spans[:2]]
        slices.append(source[last_start:viewport_end])
        plugin = (root.parent / "effects/shadows/DustShadows.cpp").read_text()
        a = plugin.index("static uint32_t GetEffectiveShadowResolution(")
        slices.append(plugin[a:plugin.index("static void ApplyDustShadows();", a)])
        output = root / "build/ShadowAtlasHooks.generated.h"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text("\n".join(slices))
    if name == "EffectOrderTests":
        source = (root.parent / "src/EffectLoader.cpp").read_text()
        begin = source.index("const std::vector<size_t>& EffectLoader::GetPostOrder(")
        end = source.index("void EffectLoader::DispatchPostLightVolumes(", begin)
        output = root / "build/EffectDispatch.generated.h"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(source[begin:end])
    if name == "EffectPresetTests":
        source = (root.parent / "src/EffectLoaderPresets.cpp").read_text()
        begin = source.index("void EffectLoader::EffectConfigLoadFrom(")
        end = source.index("void EffectLoader::ScanPresets(", begin)
        output = root / "build/EffectPresetIO.generated.h"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(source[begin:end])
    result = subprocess.run([
        msbuild, str(root / "NativeTests.vcxproj"), "/nologo", "/verbosity:minimal",
        "/p:Configuration=Release", "/p:Platform=x64", "/p:TestName=" + name,
    ], env=build_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        sys.stdout.buffer.write(result.stdout)
        raise SystemExit(result.returncode)
    subprocess.run([str(root / "build" / name / (name + ".exe"))], check=True, cwd=root)
    print(name + ": PASS", flush=True)
