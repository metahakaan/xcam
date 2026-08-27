# Collects the phone's local recordings.
#
# The stream the PC receives is compressed for a live USB link. The files this
# pulls are the other half of the deal: the same take encoded on the phone at
# whatever the sensor could give, which is the only copy worth keeping.
#
# They live in the app's own external files directory, so `adb pull` reaches them
# without root and Android removes them with the app.

param(
    [string]$Destination = (Join-Path $HOME "Videos\XCam"),
    [string]$RemoteDir = "/storage/emulated/0/Android/data/com.xcam/files/recordings",

    # Deletes each file from the phone once it has been pulled AND the local copy
    # has been verified to be the same size. Off by default: losing a take to a
    # half-finished transfer is not a recoverable mistake.
    [switch]$Remove,

    # Pull only what is not already here. Useful mid-session.
    [switch]$NewOnly
)

$ErrorActionPreference = "Stop"

# adb writes its transfer progress to stderr, and Windows PowerShell turns
# anything a native command writes there into an ErrorRecord. With Stop in force
# a perfectly successful pull would abort the script, so every adb call goes
# through this and is judged on its exit code instead.
function Invoke-Adb {
    # One array parameter rather than remaining-arguments: adb's own flags start
    # with a dash, and PowerShell would try to bind "-a" as a parameter name.
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $script:adb @Arguments 2>&1
    $script:adbExit = $LASTEXITCODE
    $ErrorActionPreference = $previous
    return $output
}

$adb = Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\adb.exe"
if (-not (Test-Path $adb)) {
    $found = Get-Command adb -ErrorAction SilentlyContinue
    if (-not $found) { throw "adb not found. Install platform-tools or add adb to PATH." }
    $adb = $found.Source
}

$devices = Invoke-Adb @("devices") | Select-Object -Skip 1 | Where-Object { $_ -match "\tdevice$" }
if (-not $devices) { throw "No device. Plug the phone in and allow USB debugging." }

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# `stat` rather than `ls -l`: one number and one name per line, in that order,
# with no locale-dependent date column in between to miscount.
$listing = Invoke-Adb @("shell", "stat -c '%s %n' $RemoteDir/*.mp4 2>/dev/null")
if (-not $listing) {
    Write-Host "No recordings in $RemoteDir."
    return
}

$pulled = 0
$skipped = 0
$totalBytes = 0L

foreach ($line in $listing) {
    # 634512890 /storage/.../XCam_20260824-131207.mp4
    if ($line -notmatch '^\s*(\d+)\s+(\S.*\.mp4)\s*$') { continue }
    $size = [int64]$Matches[1]
    $remote = $Matches[2]

    $name = Split-Path $remote -Leaf
    $local = Join-Path $Destination $name

    if ((Test-Path $local) -and $NewOnly) {
        $skipped++
        continue
    }
    if ((Test-Path $local) -and ((Get-Item $local).Length -eq $size) -and $size -gt 0) {
        Write-Host "have  $name"
        $skipped++
        continue
    }

    Write-Host ("pull  {0} ({1:N0} MB)" -f $name, ($size / 1MB))
    Invoke-Adb @("pull", "-a", $remote, $local) | Out-Null
    if ($adbExit -ne 0) {
        Write-Warning "pull failed: $name"
        continue
    }

    $localSize = (Get-Item $local).Length
    if ($size -gt 0 -and $localSize -ne $size) {
        Write-Warning "$name is $localSize bytes here but $size on the phone -- left in place."
        continue
    }

    $pulled++
    $totalBytes += $localSize

    if ($Remove) {
        Invoke-Adb @("shell", "rm -f '$remote'") | Out-Null
        Write-Host "      removed from phone"
    }
}

Write-Host ""
Write-Host ("{0} pulled, {1} already here, {2:N1} GB into {3}" -f `
            $pulled, $skipped, ($totalBytes / 1GB), $Destination)
