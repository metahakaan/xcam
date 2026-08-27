# Installs XCam for the person running it.
#
# No administrator, and that is a design decision rather than a shortcut. The
# virtual camera is a COM server, and a COM server can live under
# HKCU\Software\Classes just as well as under the machine-wide hive -- HKCR is a
# merged view of the two, so every application that enumerates cameras finds it
# either way. Nobody has to be talked past a UAC prompt to try a webcam.
#
# What it costs: the camera exists for this account only. Somebody who wants it
# for every account on the machine can run the registration elevated, and
# register-filter.ps1 says how.
#
# What this does that a copy would not:
#
#   * Puts the binaries somewhere stable. The registry records the DLL's path,
#     so a filter registered out of a build tree stops working the moment that
#     tree is moved or rebuilt somewhere else -- which is a camera that vanishes
#     for no reason a person could work out.
#   * Registers from that stable copy.
#   * Adds a Start Menu shortcut and an entry in Windows' own installed-programs
#     list, so it can be removed the way everything else is.
#
#   .\tools\install.ps1              install or update
#   .\tools\install.ps1 -Uninstall   take it all back out

param(
    [switch]$Uninstall,
    [string]$From
)

$ErrorActionPreference = 'Stop'

$appName    = 'XCam'
$installDir = Join-Path $env:LOCALAPPDATA "Programs\$appName"
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appName"
$startMenu  = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$shortcut   = Join-Path $startMenu "$appName.lnk"
$runKey     = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

# The DLL is what has to be unregistered, and it must be unregistered from
# wherever it was registered -- which is the installed copy, not the build.
function Invoke-DllEntry {
    param([string]$Dll, [string]$Entry)

    if (-not (Test-Path $Dll)) { return $false }

    # Called directly rather than through regsvr32: regsvr32 reports one opaque
    # exit code and hides the HRESULT that says what actually failed.
    #
    # The delegate is declared in the type rather than built from [Func[int]],
    # which Windows PowerShell resolves to an open generic definition and then
    # refuses to marshal.
    if (-not ('XCamInstallNative' -as [type])) {
        Add-Type @"
using System;
using System.Runtime.InteropServices;
public class XCamInstallNative {
  [DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr LoadLibraryW(string path);
  [DllImport("kernel32", SetLastError=true)]
  public static extern IntPtr GetProcAddress(IntPtr module, string name);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool FreeLibrary(IntPtr module);
  [DllImport("ole32")]
  public static extern int CoInitializeEx(IntPtr reserved, uint flags);
  [UnmanagedFunctionPointer(CallingConvention.StdCall)]
  public delegate int SelfRegister();
}
"@
    }

    # The registration talks to the filter mapper, which is COM.
    [XCamInstallNative]::CoInitializeEx([IntPtr]::Zero, 2) | Out-Null

    $module = [XCamInstallNative]::LoadLibraryW((Resolve-Path $Dll).Path)
    if ($module -eq [IntPtr]::Zero) { throw "could not load $Dll" }

    try {
        $proc = [XCamInstallNative]::GetProcAddress($module, $Entry)
        if ($proc -eq [IntPtr]::Zero) { throw "$Dll has no $Entry" }

        $call = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
                    $proc, [XCamInstallNative+SelfRegister])
        $hr = $call.Invoke()
        if ($hr -ne 0) { throw ("{0} returned 0x{1:x8}" -f $Entry, $hr) }
        return $true
    } finally {
        [XCamInstallNative]::FreeLibrary($module) | Out-Null
    }
}

if ($Uninstall) {
    $installedDll = Join-Path $installDir 'xcam-dsfilter.dll'
    if (Test-Path $installedDll) {
        Write-Host 'unregistering the virtual devices'
        try { Invoke-DllEntry -Dll $installedDll -Entry 'DllUnregisterServer' | Out-Null }
        catch { Write-Warning $_.Exception.Message }
    }

    # The autostart entry goes with it. Leaving one behind means a boot that
    # tries to run something that is no longer there.
    if (Get-ItemProperty -Path $runKey -Name $appName -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -Path $runKey -Name $appName
        Write-Host 'removed the startup entry'
    }

    if (Test-Path $shortcut) { Remove-Item $shortcut }
    if (Test-Path $uninstallKey) { Remove-Item $uninstallKey -Recurse }

    if (Test-Path $installDir) {
        # The application may still be running; say so rather than failing
        # halfway through with a locked file.
        Get-Process xcam-app -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Warning 'XCam is running; close it and run this again to remove the files'
            exit 1
        }
        Remove-Item $installDir -Recurse -Force
    }

    Write-Host "$appName removed. Settings and recordings were left alone."
    exit 0
}

# ---- install ----------------------------------------------------------------

# Two places worth looking, in the order they are likely.
#
# Beside this script first: that is a downloaded release, where the installer
# and the binaries are unzipped into one folder and there is no build tree at
# all. Then the build tree, which is where it sits when run from a clone.
if (-not $From) {
    if (Test-Path (Join-Path $PSScriptRoot 'xcam-app.exe')) {
        $From = $PSScriptRoot
    } else {
        $From = Join-Path (Split-Path $PSScriptRoot -Parent) 'windows\build\bin\Release'
    }
}
if (-not (Test-Path $From)) {
    throw "no binaries at $From. Build first with tools\build-windows.ps1, or pass -From"
}

$needed = @('xcam-app.exe', 'xcam-dsfilter.dll', 'xcam-probe.exe')
foreach ($file in $needed) {
    if (-not (Test-Path (Join-Path $From $file))) { throw "$From is missing $file" }
}

# An existing installation is unregistered before its files are replaced. The
# registry records the path; overwriting the DLL underneath a live registration
# leaves entries pointing at a file that has changed identity.
$installedDll = Join-Path $installDir 'xcam-dsfilter.dll'
if (Test-Path $installedDll) {
    Write-Host 'updating an existing installation'
    try { Invoke-DllEntry -Dll $installedDll -Entry 'DllUnregisterServer' | Out-Null }
    catch { Write-Warning $_.Exception.Message }
}

Get-Process xcam-app -ErrorAction SilentlyContinue | ForEach-Object {
    throw 'XCam is running. Close it first.'
}

New-Item -ItemType Directory -Force -Path $installDir | Out-Null
foreach ($file in $needed) {
    Copy-Item (Join-Path $From $file) $installDir -Force
}
Write-Host "installed to $installDir"

Write-Host 'registering the virtual camera and microphone'
Invoke-DllEntry -Dll $installedDll -Entry 'DllRegisterServer' | Out-Null

# The Start Menu entry, so it can be found the way anything else is.
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = Join-Path $installDir 'xcam-app.exe'
$link.WorkingDirectory = $installDir
$link.Description = 'Use a phone as a camera'
$link.Save()

# And Windows' own list, so removing it does not require finding this script.
New-Item -Path $uninstallKey -Force | Out-Null
$version = (Get-Item (Join-Path $installDir 'xcam-app.exe')).VersionInfo.FileVersion
if (-not $version) { $version = '1.0' }

$quotedScript = '"' + (Join-Path $PSScriptRoot 'install.ps1') + '"'
Set-ItemProperty -Path $uninstallKey -Name 'DisplayName'     -Value $appName
Set-ItemProperty -Path $uninstallKey -Name 'DisplayVersion'  -Value $version
Set-ItemProperty -Path $uninstallKey -Name 'Publisher'       -Value $appName
Set-ItemProperty -Path $uninstallKey -Name 'InstallLocation' -Value $installDir
Set-ItemProperty -Path $uninstallKey -Name 'DisplayIcon'     -Value (Join-Path $installDir 'xcam-app.exe')
Set-ItemProperty -Path $uninstallKey -Name 'NoModify'        -Value 1 -Type DWord
Set-ItemProperty -Path $uninstallKey -Name 'NoRepair'        -Value 1 -Type DWord
Set-ItemProperty -Path $uninstallKey -Name 'UninstallString' `
    -Value "powershell -NoProfile -ExecutionPolicy Bypass -File $quotedScript -Uninstall"

Write-Host ''
Write-Host "$appName is installed for $env:USERNAME."
Write-Host 'The camera and microphone are in every application''s device list now.'
Write-Host 'Install the phone app with tools\build-android.ps1 -Install, then press start on it.'
