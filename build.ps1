param(
    [switch]$Deploy,
    [switch]$Package
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

# Build boot (preload plugin)
Build-Project (Join-Path $Root "boot\DustBoot.vcxproj")

# Build host
Build-Project (Join-Path $Root "src\Dust.vcxproj")

$Effects = @("ssao", "lut", "bloom", "dof", "ssil", "clarity", "outline", "kuwahara", "rtgi", "shadows", "smaa", "chromaticaberration", "deband", "filmgrain", "letterbox", "vignette")
foreach ($effect in $Effects) {
    $vcxproj = Get-ChildItem (Join-Path $Root "effects\$effect\*.vcxproj") | Select-Object -First 1
    Build-Project $vcxproj.FullName
}

Write-Host "==> Build complete" -ForegroundColor Green

if (-not $Deploy -and -not $Package) { exit 0 }

# Copy the built mod into $Dest (a Kenshi mod folder for -Deploy, or a repo-local
# ./Dust folder for -Package). -Clean wipes $Dest first so a package can't carry stale
# files from a previous build (an effect that was renamed/removed, etc.).
function Publish-Build($Dest, [switch]$Clean) {
    if ($Clean -and (Test-Path $Dest)) {
        Write-Host "    cleaning $Dest" -ForegroundColor DarkGray
        Remove-Item -Recurse -Force $Dest
    }
    New-Item -ItemType Directory -Force -Path "$Dest\effects\shaders" | Out-Null
    New-Item -ItemType Directory -Force -Path "$Dest\presets"         | Out-Null

    Copy-Item "$Root\boot\build\Release\DustBoot.dll" "$Dest\"
    Copy-Item "$Root\src\build\Release\Dust.dll"  "$Dest\"
    Copy-Item "$Root\mod\RE_Kenshi.json"          "$Dest\"
    Copy-Item "$Root\mod\Dust.mod"                "$Dest\"

    # DLSS runtime model — only when the NGX SDK is vendored (external/DLSS is gitignored). NGX loads it
    # from the mod dir via the path Upscaler::Init passes.
    $NgxDll = "$Root\external\DLSS\lib\Windows_x86_64\rel\nvngx_dlss.dll"
    if (Test-Path $NgxDll) { Copy-Item $NgxDll "$Dest\"; Write-Host "    + nvngx_dlss.dll (DLSS runtime)" -ForegroundColor DarkGray }

    # FSR3/FSR4 ffx-api runtime — only when the FidelityFX SDK is vendored (external/FidelityFX-SDK is
    # gitignored). The loader dispatches to the upscaler provider; both must sit next to Dust.dll.
    $FfxBin = "$Root\external\FidelityFX-SDK\Kits\FidelityFX\signedbin"
    foreach ($ffxDll in @("amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll")) {
        if (Test-Path "$FfxBin\$ffxDll") { Copy-Item "$FfxBin\$ffxDll" "$Dest\"; Write-Host "    + $ffxDll (FSR3/4 runtime)" -ForegroundColor DarkGray }
    }

    foreach ($effect in $Effects) {
        $dll = Get-ChildItem "$Root\effects\$effect\build\Release\Dust*.dll" | Select-Object -First 1
        Copy-Item $dll.FullName "$Dest\effects\"
        $shaders = Get-ChildItem "$Root\effects\$effect\shaders\*.hlsl" -ErrorAction SilentlyContinue
        if ($shaders) { Copy-Item $shaders.FullName "$Dest\effects\shaders\" }
    }

    $presets = Get-ChildItem "$Root\effects\presets\*" -ErrorAction SilentlyContinue
    if ($presets) { Copy-Item "$Root\effects\presets\*" "$Dest\presets\" -Recurse -Force }
}

if ($Deploy) {
    Write-Host "==> Deploying to $ModDir" -ForegroundColor Cyan
    Publish-Build $ModDir
    Write-Host "==> Deploy complete" -ForegroundColor Green
}

if ($Package) {
    $PackageDir = Join-Path $Root "Dust"
    Write-Host "==> Packaging to $PackageDir" -ForegroundColor Cyan
    Publish-Build $PackageDir -Clean
    Write-Host "==> Package complete: $PackageDir" -ForegroundColor Green
}
