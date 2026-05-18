param(
    [switch]$Deploy,
    [switch]$SkipPresets,
    [switch]$Tracy
)

$ErrorActionPreference = "Stop"

$MSBUILD = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
$Root = $PSScriptRoot

# -Tracy enables Tracy profiling. Sets the ExtraDefines env var which the
# vcxprojs forward into PreprocessorDefinitions via $(ExtraDefines). Cannot
# pass via /p: because the semicolons get parsed as MSBuild switch separators.
# Includes TRACY_ON_DEMAND so the listener only spins up once a server
# connects (~zero idle cost) plus the standard "minimal overhead" flags.
if ($Tracy) {
    $env:ExtraDefines = "TRACY_ENABLE;TRACY_ON_DEMAND;TRACY_NO_SAMPLING;TRACY_NO_SYSTEM_TRACING;TRACY_NO_CALLSTACK;TRACY_NO_CALLSTACK_INLINES;TRACY_NO_CRASH_HANDLER;TRACY_NO_VSYNC_CAPTURE;TRACY_NO_FRAME_IMAGE"
    Write-Host "==> Tracy build (TRACY_ENABLE + on-demand)" -ForegroundColor Yellow
}

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

# Build boot (preload plugin)
Build-Project (Join-Path $Root "boot\DustBoot.vcxproj")

# Build host
Build-Project (Join-Path $Root "src\Dust.vcxproj")

$Effects = @("ssao", "ssaodebug", "lut", "bloom", "dof", "ssil", "clarity", "outline", "kuwahara", "rtgi", "shadows", "smaa", "chromaticaberration", "deband", "filmgrain", "letterbox", "vignette", "pom", "terraintess")
foreach ($effect in $Effects) {
    $vcxproj = Get-ChildItem (Join-Path $Root "effects\$effect\*.vcxproj") | Select-Object -First 1
    Build-Project $vcxproj.FullName
}

Write-Host "==> Build complete" -ForegroundColor Green

if (-not $Deploy) { exit 0 }

Write-Host "==> Deploying to $ModDir" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path "$ModDir\effects\shaders" | Out-Null
if (-not $SkipPresets) {
    New-Item -ItemType Directory -Force -Path "$ModDir\presets"     | Out-Null
}

# Prune stale Dust*.dll from previous deploys (sibling branches, removed
# plugins). Plugins built against an older DustAPI version can crash the host
# loader. Scoped to effects/ so the root Dust.dll is unaffected. .ini files
# are left alone — they hold user settings and don't load on their own.
$expectedDlls = @{}
foreach ($effect in $Effects) {
    $src = Get-ChildItem "$Root\effects\$effect\build\Release\Dust*.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($src) { $expectedDlls[$src.Name] = $true }
}
Get-ChildItem "$ModDir\effects\Dust*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
    if (-not $expectedDlls.ContainsKey($_.Name)) {
        Write-Host "==> Pruning stale plugin: $($_.Name)" -ForegroundColor Yellow
        Remove-Item $_.FullName -Force
    }
}

Copy-Item "$Root\boot\build\Release\DustBoot.dll" "$ModDir\"
Copy-Item "$Root\src\build\Release\Dust.dll"  "$ModDir\"
Copy-Item "$Root\mod\RE_Kenshi.json"          "$ModDir\"
Copy-Item "$Root\mod\Dust.mod"                "$ModDir\"

# Heightmaps for terrain tessellation displacement
if (Test-Path "$Root\mod\heightmaps") {
    New-Item -ItemType Directory -Force -Path "$ModDir\heightmaps" | Out-Null
    Copy-Item "$Root\mod\heightmaps\*.bin" "$ModDir\heightmaps\" -Force -ErrorAction SilentlyContinue
}

foreach ($effect in $Effects) {
    $dll = Get-ChildItem "$Root\effects\$effect\build\Release\Dust*.dll" | Select-Object -First 1
    Copy-Item $dll.FullName "$ModDir\effects\"
    $shaders = Get-ChildItem "$Root\effects\$effect\shaders\*.hlsl","$Root\effects\$effect\shaders\*.hlsli" -ErrorAction SilentlyContinue
    if ($shaders) { Copy-Item $shaders.FullName "$ModDir\effects\shaders\" }
}

if ($SkipPresets) {
    Write-Host "==> Skipping presets (per -SkipPresets)" -ForegroundColor Yellow
} else {
    $presets = Get-ChildItem "$Root\effects\presets\*" -ErrorAction SilentlyContinue
    if ($presets) { Copy-Item "$Root\effects\presets\*" "$ModDir\presets\" -Recurse -Force }
}

Write-Host "==> Deploy complete" -ForegroundColor Green
