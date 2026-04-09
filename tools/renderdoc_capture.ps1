param(
    [string]$KenshiRoot = "D:\SteamLibrary\steamapps\common\Kenshi",
    [string]$RenderDocCmd = "C:\Program Files\RenderDoc\renderdoccmd.exe",
    [string]$CapturePrefix = "D:\Github\kenshi Graphics Upgrade\captures\kenshi_capture",
    [int]$TimeoutSeconds = 30,
    [switch]$LaunchKenshi
)

$targetPath = (Join-Path $KenshiRoot "RE_Kenshi\kenshi_x64.exe").ToLowerInvariant()
$launcherPath = Join-Path $KenshiRoot "kenshi_x64.exe"

Write-Host "Waiting for Kenshi child process under: $KenshiRoot"
$launchInfoShown = $false

if ($LaunchKenshi) {
    if (-not (Test-Path -LiteralPath $launcherPath)) {
        Write-Error "Launcher executable not found: $launcherPath"
        exit 1
    }

    Write-Host "Launching Kenshi launcher: $launcherPath"
    Start-Process -FilePath $launcherPath -WorkingDirectory $KenshiRoot | Out-Null
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$matched = $null
$seen = @{}

while ((Get-Date) -lt $deadline) {
    $kenshiProcesses = Get-CimInstance Win32_Process |
        Where-Object {
            $_.Name -ieq "kenshi_x64.exe" -and
            $_.ExecutablePath -and
            $_.ExecutablePath.ToLowerInvariant().StartsWith($KenshiRoot.ToLowerInvariant())
        }

    foreach ($proc in $kenshiProcesses) {
        $key = "$($proc.ProcessId)|$($proc.ExecutablePath)"
        if (-not $seen.ContainsKey($key)) {
            $seen[$key] = $true
            Write-Host ("Detected PID {0}: {1}" -f $proc.ProcessId, $proc.ExecutablePath)
        }
    }

    $matched = $kenshiProcesses |
        Where-Object { $_.ExecutablePath.ToLowerInvariant() -eq $targetPath } |
        Select-Object -First 1

    if ($matched) {
        break
    }

    if (-not $launchInfoShown) {
        Write-Host "If the launcher is open, click OK to start the actual game process."
        $launchInfoShown = $true
    }

    Start-Sleep -Milliseconds 100
}

if (-not $matched) {
    Write-Error "Timed out waiting for child process $targetPath"
    exit 1
}

Write-Host "Injecting RenderDoc into PID $($matched.ProcessId)"
& $RenderDocCmd inject --PID=$($matched.ProcessId) -c $CapturePrefix --opt-delay-for-debugger 0
$exitCode = $LASTEXITCODE
Write-Host "renderdoccmd exit code: $exitCode"
exit $exitCode
