"""Exercise the actual MSBuild fingerprint target using an isolated source tree."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from xml.sax.saxutils import escape


class ShaderFingerprintTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.inputs = [
            "src/ShaderPatch.cpp", "src/ShaderPatch.h", "src/MotionVectors.cpp",
            "src/MotionVectors.h", "src/D3D11Hook.cpp", "src/D3D11Hook.h", "src/DustAPI.h",
            "src/ShadowCasterBias.h",
            "effects/shadows/DustShadows.cpp", "effects/shadows/layout.h",
            "effects/shadows/shaders/shadow.hlsl",
        ]
        for name in self.inputs:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("original " + name)
        target = Path(__file__).resolve().parents[1] / "build_support/ShaderFingerprint.targets"
        self.project = self.root / "fixture.proj"
        self.project.write_text(f'''<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<PropertyGroup><ShaderFingerprintRoot>{escape(str(self.root))}/</ShaderFingerprintRoot>
<ShaderFingerprintDir>{escape(str(self.root))}/out/</ShaderFingerprintDir></PropertyGroup>
<Import Project="{escape(str(target))}" /></Project>''')
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        self.msbuild = subprocess.check_output([
            str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.Component.MSBuild",
            "-find", "MSBuild/**/Bin/amd64/MSBuild.exe",
        ], text=True).strip().splitlines()[0]

    def generate(self):
        result = subprocess.run([
            self.msbuild, str(self.project), "/t:GenerateShaderFingerprint", "/nologo", "/verbosity:quiet",
        ], env={k.upper(): v for k, v in os.environ.items()}, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout.decode(errors="replace"))
        return (self.root / "out/ShaderFingerprint.generated.h").read_bytes()

    def test_unchanged_build_and_unrelated_source_preserve_header(self):
        original = self.generate()
        header = self.root / "out/ShaderFingerprint.generated.h"
        mtime = header.stat().st_mtime_ns
        (self.root / "src/DustGUI.cpp").write_text("unrelated UI edit")
        self.assertEqual(original, self.generate())
        self.assertEqual(mtime, header.stat().st_mtime_ns)

    def test_each_injection_or_layout_input_invalidates_without_version_bump(self):
        previous = self.generate()
        for name in self.inputs:
            with self.subTest(input=name):
                path = self.root / name
                path.write_text(path.read_text() + " changed")
                current = self.generate()
                self.assertNotEqual(previous, current)
                previous = current


if __name__ == "__main__":
    unittest.main()
