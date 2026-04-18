param(
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"

$MSBUILD = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
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

# Build host
Build-Project (Join-Path $Root "src\Dust.vcxproj")

# Build effect plugins (SSS excluded — not release-ready)
$Effects = @("ssao", "lut", "bloom", "dof", "ssil", "clarity", "outline", "kuwahara", "rtgi")
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

Copy-Item "$Root\src\build\Release\Dust.dll"  "$ModDir\"
Copy-Item "$Root\mod\RE_Kenshi.json"          "$ModDir\"
Copy-Item "$Root\mod\Dust.mod"                "$ModDir\"
Copy-Item "$Root\mod\shaders\deferred.hlsl"   "$ModDir\shaders\"

foreach ($effect in $Effects) {
    $dll = Get-ChildItem "$Root\effects\$effect\build\Release\Dust*.dll" | Select-Object -First 1
    Copy-Item $dll.FullName "$ModDir\effects\"
    $shaders = Get-ChildItem "$Root\effects\$effect\shaders\*.hlsl" -ErrorAction SilentlyContinue
    if ($shaders) { Copy-Item $shaders.FullName "$ModDir\effects\shaders\" }
}

$presets = Get-ChildItem "$Root\effects\presets\*" -ErrorAction SilentlyContinue
if ($presets) { Copy-Item "$Root\effects\presets\*" "$ModDir\presets\" -Recurse -Force }

Write-Host "==> Deploy complete" -ForegroundColor Green
