# Builds the Windows side: xcam-core, xcam-probe and xcam-app.
#
# Needs nothing but Visual Studio and the Windows SDK -- no vendored libraries,
# no package manager. The decoder is Media Foundation, the renderer is D3D11,
# and the handshake parser is hand-rolled for exactly that reason.

param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$Run
)

$ErrorActionPreference = "Stop"

$root      = Split-Path -Parent $PSScriptRoot
$sourceDir = Join-Path $root "windows"
$buildDir  = Join-Path $sourceDir "build"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

# Pick the newest generator this machine actually has rather than pinning one.
$generator = $null
foreach ($candidate in @(
    @{ Name = "Visual Studio 18 2026"; Path = "C:\Program Files\Microsoft Visual Studio\18" },
    @{ Name = "Visual Studio 17 2022"; Path = "C:\Program Files\Microsoft Visual Studio\2022" }
)) {
    if (Test-Path $candidate.Path) { $generator = $candidate.Name; break }
}
if (-not $generator) { throw "No Visual Studio installation found." }

Write-Host "generator : $generator"
Write-Host "config    : $Config"
Write-Host ""

$prev = $ErrorActionPreference
$ErrorActionPreference = "Continue"

cmake -S $sourceDir -B $buildDir -G $generator -A x64
if ($LASTEXITCODE -ne 0) { $ErrorActionPreference = $prev; throw "CMake configure failed." }

cmake --build $buildDir --config $Config
$code = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($code -ne 0) { throw "Build failed with exit code $code." }

$binDir = Join-Path $buildDir "bin\$Config"
Write-Host ""
Get-ChildItem -Path $binDir -Filter *.exe | ForEach-Object {
    Write-Host ("{0}  ({1:N0} KB)" -f $_.FullName, ($_.Length / 1KB))
}

if ($Run) {
    Start-Process (Join-Path $binDir "xcam-app.exe")
}
