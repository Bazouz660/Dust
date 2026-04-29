# One-shot fetch for the external dependencies that don't come down via
# `git submodule update`:
#   - KenshiLib.lib    (downloaded from the KenshiReclaimer/KenshiLib release
#                      that matches the tag of the KenshiLib submodule)
#   - Boost 1.60 headers (downloaded from archives.boost.io)
#
# Run once after cloning + `GIT_LFS_SKIP_SMUDGE=1 git submodule update --init
# --recursive`. Skips work that is already in place.

param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$repoRoot = (git rev-parse --show-toplevel) 2>$null
if (-not $repoRoot) { throw "Not inside a git repository." }
Set-Location $repoRoot

$libsDir   = Join-Path $repoRoot "external\KenshiLib_Examples_deps\KenshiLib\Libraries"
$boostDir  = Join-Path $repoRoot "external\KenshiLib_Examples_deps\boost_1_60_0"
$libPath   = Join-Path $libsDir "KenshiLib.lib"
$boostHdrs = Join-Path $boostDir "boost"

# ----- KenshiLib.lib -----
$tag = (git -C external/KenshiLib describe --tags --exact-match HEAD) 2>$null
if (-not $tag) {
    throw "KenshiLib submodule HEAD is not on a tagged release. Pin it to a release tag (e.g. v0.2.1) so the .lib can be fetched from KenshiReclaimer/KenshiLib's release assets."
}
$tag = $tag.Trim()

if ((Test-Path $libPath) -and -not $Force) {
    Write-Host "KenshiLib.lib already in place ($libPath); skipping. Pass -Force to redownload."
} else {
    $url = "https://github.com/KenshiReclaimer/KenshiLib/releases/download/$tag/KenshiLib_$tag.zip"
    $zip = Join-Path $env:TEMP "KenshiLib_$tag.zip"
    Write-Host "Downloading KenshiLib $tag from $url"
    & curl.exe -fL --ssl-no-revoke -o $zip $url
    if ($LASTEXITCODE -ne 0) { throw "Failed to download KenshiLib release zip." }

    if (-not (Test-Path $libsDir)) { New-Item -ItemType Directory -Path $libsDir | Out-Null }
    & tar.exe -xf $zip -C $libsDir KenshiLib.lib
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract KenshiLib.lib from $zip." }
    Write-Host "Placed $libPath"
}

# ----- Boost 1.60 headers -----
if ((Test-Path $boostHdrs) -and -not $Force) {
    Write-Host "Boost headers already in place ($boostHdrs); skipping. Pass -Force to redownload."
} else {
    $url = "https://archives.boost.io/release/1.60.0/source/boost_1_60_0.tar.bz2"
    $tar = Join-Path $env:TEMP "boost_1_60_0.tar.bz2"
    Write-Host "Downloading Boost 1.60 from $url (~73 MB)"
    & curl.exe -fL --ssl-no-revoke -o $tar $url
    if ($LASTEXITCODE -ne 0) { throw "Failed to download Boost." }

    if (-not (Test-Path $boostDir)) { New-Item -ItemType Directory -Path $boostDir | Out-Null }
    if (Test-Path $boostHdrs) { Remove-Item -Recurse -Force $boostHdrs }

    $work = Join-Path $env:TEMP ("boost-extract-" + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        & tar.exe -xjf $tar -C $work boost_1_60_0/boost
        if ($LASTEXITCODE -ne 0) { throw "Failed to extract Boost archive." }
        Move-Item (Join-Path $work "boost_1_60_0\boost") $boostHdrs
    } finally {
        Remove-Item -Recurse -Force $work
    }
    Write-Host "Placed $boostHdrs"
}

Write-Host ""
Write-Host "Done. You can now build with build.ps1."
