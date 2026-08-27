# Registers (or unregisters) the XCam virtual devices with Windows.
#
# One DLL carries both the camera and the microphone, so one registration puts
# both in every application's device list.
#
# Calls DllRegisterServer directly rather than going through regsvr32. regsvr32
# reports a single opaque exit code, hides the HRESULT that says what actually
# failed, and does not cope with the non-ASCII path this project lives under.
#
# Registration falls back to per-user (HKCU\Software\Classes) when it cannot
# write the machine-wide keys, which is the normal case without elevation and is
# enough for the person who installed it. Elevate only if every account on the
# machine should see the camera.
#
# The DLL's path is recorded in the registry, so it must stay where it is:
# moving or rebuilding into a different directory leaves applications unable to
# load it.

param(
    [switch]$Unregister,
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root "windows\build\bin\$Config\xcam-dsfilter.dll"

if (-not (Test-Path $dll)) {
    throw "Filter not built: $dll`nRun tools\build-windows.ps1 first."
}
$dll = (Resolve-Path $dll).Path

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class XCamReg {
  [DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr LoadLibraryW(string path);
  [DllImport("kernel32", SetLastError=true)]
  public static extern IntPtr GetProcAddress(IntPtr module, string name);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool FreeLibrary(IntPtr module);
  [DllImport("ole32.dll")]
  public static extern int CoInitializeEx(IntPtr reserved, uint flags);
  [UnmanagedFunctionPointer(CallingConvention.StdCall)]
  public delegate int SelfRegister();
}
"@

[XCamReg]::CoInitializeEx([IntPtr]::Zero, 2) | Out-Null

$module = [XCamReg]::LoadLibraryW($dll)
if ($module -eq [IntPtr]::Zero) {
    throw "Could not load $dll (win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
}

$entry = if ($Unregister) { "DllUnregisterServer" } else { "DllRegisterServer" }
$address = [XCamReg]::GetProcAddress($module, $entry)
if ($address -eq [IntPtr]::Zero) { throw "$entry not exported by the DLL." }

$fn = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($address, [XCamReg+SelfRegister])
$hr = $fn.Invoke()

if ($hr -ne 0) {
    throw ("{0} failed with HRESULT 0x{1:x8}" -f $entry, $hr)
}

$clsid = "{8CACBF3B-D1C2-488A-B8DB-3A6CB3E9905A}"
$category = "{860BB310-5D01-11D0-BD3B-00A0C911CE86}"   # CLSID_VideoInputDeviceCategory
$micClsid = "{B4E7A1D6-3F52-4C88-9A0E-7D61C2F84B13}"
$micCategory = "{33D9A762-90C8-11D0-BD43-00A0C911CE86}" # CLSID_AudioInputDeviceCategory

if ($Unregister) {
    Write-Host "Unregistered."
    return
}

# Verify rather than trust: a DllRegisterServer can return S_OK having written
# only half of what an application needs to find the device.
$comKey = $null
foreach ($candidate in @("HKCU:\Software\Classes\CLSID\$clsid\InprocServer32",
                         "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32")) {
    if (Test-Path $candidate) { $comKey = $candidate; break }
}

$deviceKey = $null
foreach ($candidate in @("HKCU:\Software\Classes\CLSID\$category\Instance\$clsid",
                         "HKLM:\SOFTWARE\Classes\CLSID\$category\Instance\$clsid")) {
    if (Test-Path $candidate) { $deviceKey = $candidate; break }
}

if (-not $comKey)    { Write-Warning "No COM server entry was written." }
if (-not $deviceKey) { Write-Warning "No video-input-device entry was written." }

if ($comKey) {
    $registeredPath = (Get-ItemProperty $comKey)."(default)"
    Write-Host "COM server : $comKey"
    if ($registeredPath -ne $dll) {
        Write-Warning "Registered path is $registeredPath, not $dll."
    }
}
if ($deviceKey) {
    $scope = if ($deviceKey.StartsWith("HKCU")) { "this user" } else { "all users" }
    Write-Host "Device     : $deviceKey ($scope)"
}

$micKey = $null
foreach ($candidate in @("HKCU:\Software\Classes\CLSID\$micCategory\Instance\$micClsid",
                         "HKLM:\SOFTWARE\Classes\CLSID\$micCategory\Instance\$micClsid")) {
    if (Test-Path $candidate) { $micKey = $candidate; break }
}
if (-not $micKey) { Write-Warning "No audio-input-device entry was written." }
else {
    $scope = if ($micKey.StartsWith("HKCU")) { "this user" } else { "all users" }
    Write-Host "Microphone : $micKey ($scope)"
}

Write-Host ""
Write-Host 'Registered as "XCam Virtual Camera" and "XCam Virtual Microphone".'
Write-Host "Start xcam-app.exe to give them something to carry."
