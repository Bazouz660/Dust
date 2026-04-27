param(
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"

$MSBUILD = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
if (-not (Test-Path $MSBUILD)) {
    $MSBUILD = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
}
if (-not (Test-Path $MSBUILD)) {
    Write-Error "ERROR: MSBuild.exe not found. Update build.ps1 with the correct path."
}
$Root = $PSScriptRoot

if ($Deploy) {
    $EnvFile = Join-Path $Root ".env"
    if (-not (Test-Path $EnvFile)) {
        Write-Error "ERROR: .env file not found at $EnvFile`nCreate it with: KENSHI_MOD_DIR=C:\path\to\kenshi\mods\Dust"
    }
    foreach ($line in Get-Content $EnvFile) {
        if ($line -match "^\s*KENSHI_MOD_DIR\s*=\s*(.+)") {
            $ModDir = $Matches[1].Trim()
        }
    }
    if (-not $ModDir) {
        Write-Error "ERROR: KENSHI_MOD_DIR not set in .env"
    }
}

function Build-Project($vcxproj) {
    $name = Split-Path $vcxproj -Leaf
    Write-Host "==> Building $name" -ForegroundColor Cyan
    & $MSBUILD $vcxproj /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $name" }
}

# Pre-build math validation: ensures the sun-frustum matrix math in DPMRenderer
# stays correct. Fails the build if the roundtrip / identity assertions break.
Write-Host "==> Validating sun-frustum math" -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root "tools\validate_sun_frustum.ps1") | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Error "Sun-frustum math validation FAILED. Run tools\validate_sun_frustum.ps1 directly to see details."
}

# Build boot (preload plugin)
Build-Project (Join-Path $Root "boot\DustBoot.vcxproj")

# Build host
Build-Project (Join-Path $Root "src\Dust.vcxproj")

# Build effect plugins (SSS excluded — not release-ready)
$Effects = @("ssao", "lut", "bloom", "dof", "ssil", "clarity", "outline", "kuwahara", "rtgi", "shadows", "dustshadows-rt", "smaa", "chromaticaberration", "deband", "filmgrain", "letterbox", "vignette", "debugviews")
foreach ($effect in $Effects) {
    $vcxproj = Get-ChildItem (Join-Path $Root "effects\$effect\*.vcxproj") | Select-Object -First 1
    Build-Project $vcxproj.FullName
}

Write-Host "==> Build complete" -ForegroundColor Green

if (-not $Deploy) { exit 0 }

Write-Host "==> Deploying to $ModDir" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path "$ModDir\effects\shaders" | Out-Null
New-Item -ItemType Directory -Force -Path "$ModDir\shaders"         | Out-Null
New-Item -ItemType Directory -Force -Path "$ModDir\presets"         | Out-Null

Copy-Item "$Root\boot\build\Release\DustBoot.dll" "$ModDir\"
Copy-Item "$Root\src\build\Release\Dust.dll"  "$ModDir\"
Copy-Item "$Root\mod\RE_Kenshi.json"          "$ModDir\"
Copy-Item "$Root\mod\Dust.mod"                "$ModDir\"

foreach ($effect in $Effects) {
    $dll = Get-ChildItem "$Root\effects\$effect\build\Release\Dust*.dll" | Select-Object -First 1
    Copy-Item $dll.FullName "$ModDir\effects\"
    $shaders = Get-ChildItem "$Root\effects\$effect\shaders\*.hlsl" -ErrorAction SilentlyContinue
    if ($shaders) { Copy-Item $shaders.FullName "$ModDir\effects\shaders\" }
}

$presets = Get-ChildItem "$Root\effects\presets\*" -ErrorAction SilentlyContinue
if ($presets) { Copy-Item "$Root\effects\presets\*" "$ModDir\presets\" -Recurse -Force }

Write-Host "==> Deploy complete" -ForegroundColor Green
