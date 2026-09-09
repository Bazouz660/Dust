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
    result = subprocess.run([
        msbuild, str(root / "NativeTests.vcxproj"), "/nologo", "/verbosity:minimal",
        "/p:Configuration=Release", "/p:Platform=x64", "/p:TestName=" + name,
    ], env=build_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        sys.stdout.buffer.write(result.stdout)
        raise SystemExit(result.returncode)
    subprocess.run([str(root / "build" / name / (name + ".exe"))], check=True, cwd=root)
    print(name + ": PASS", flush=True)
