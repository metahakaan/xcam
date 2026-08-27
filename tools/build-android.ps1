# Builds the XCam APK.
#
# Two things this machine needs and a bare `gradle` invocation does not give you:
#
#  1. JAVA_HOME pointed at Android Studio's JBR. The system Java (Oracle jre-1.8)
#     is broken -- its jvm.cfg is missing -- so anything picking Java off PATH dies.
#  2. -Djdk.net.unixdomain.tmpdir moved out of %LOCALAPPDATA%\Temp. AF_UNIX socket
#     files cannot be connected to inside that folder here, and the JDK backs every
#     Selector with one, so without this the Gradle daemon cannot even start
#     ("Unable to establish loopback connection"). gradle.properties carries the
#     same flag for the daemon; GRADLE_OPTS covers the launcher JVM.

param(
    [string]$Task = "assembleDebug",
    [switch]$Install
)

$ErrorActionPreference = "Stop"

$root      = Split-Path -Parent $PSScriptRoot
$androidDir = Join-Path $root "android"
$sockDir   = Join-Path $env:USERPROFILE ".javasock"
$gradleVer = "9.1.0"

$env:JAVA_HOME   = "C:\Program Files\Android\Android Studio\jbr"
$env:ANDROID_HOME = Join-Path $env:LOCALAPPDATA "Android\Sdk"

if (-not (Test-Path $env:JAVA_HOME)) {
    throw "JBR not found at $env:JAVA_HOME. Install Android Studio or edit this script."
}
New-Item -ItemType Directory -Force -Path $sockDir | Out-Null

# JAVA_TOOL_OPTIONS rather than GRADLE_OPTS or org.gradle.jvmargs: Gradle strips
# -D flags out of jvmargs and re-applies them with System.setProperty once the
# daemon is up, which is far too late -- the JDK reads this one in a static
# initialiser. JAVA_TOOL_OPTIONS is read by the JVM itself at startup and is
# inherited by every child (Gradle daemon, Kotlin daemon, aapt2), so one variable
# covers the whole build.
#
# The value is split on whitespace, so the path must be in 8.3 form --
# "C:\Users\Taha Kaan\..." would arrive as two arguments.
$fso = New-Object -ComObject Scripting.FileSystemObject
$sockShort = $fso.GetFolder($sockDir).ShortPath
$env:JAVA_TOOL_OPTIONS = "-Djdk.net.unixdomain.tmpdir=$sockShort"

# Prefer the wrapper; fall back to a Gradle distribution already in the cache so
# a clean checkout builds without downloading anything.
$gradleCmd = Join-Path $androidDir "gradlew.bat"
if (-not (Test-Path $gradleCmd)) {
    $dist = Get-ChildItem -Path (Join-Path $env:USERPROFILE ".gradle\wrapper\dists") `
                          -Filter "gradle-$gradleVer" -Recurse -Directory -ErrorAction SilentlyContinue |
            Select-Object -First 1
    if (-not $dist) { throw "No gradlew.bat and no cached Gradle $gradleVer. Run: gradle wrapper" }
    $gradleCmd = Join-Path $dist.FullName "bin\gradle.bat"
}

Write-Host "gradle : $gradleCmd"
Write-Host "java   : $env:JAVA_HOME"
Write-Host "sdk    : $env:ANDROID_HOME"
Write-Host ""

Push-Location $androidDir
try {
    # Windows PowerShell turns anything a native command writes to stderr into an
    # ErrorRecord, and with ErrorActionPreference=Stop the JVM's harmless
    # "Picked up JAVA_TOOL_OPTIONS" notice would abort the script. Exit code is
    # the only trustworthy signal here.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $gradleCmd $Task --console=plain
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev

    if ($code -ne 0) { throw "Gradle failed with exit code $code" }
}
finally { Pop-Location }

$apk = Join-Path $androidDir "app\build\outputs\apk\debug\app-debug.apk"
if (Test-Path $apk) {
    $size = [math]::Round((Get-Item $apk).Length / 1MB, 2)
    Write-Host ""
    Write-Host "APK: $apk ($size MB)"

    if ($Install) {
        $adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
        # -g grants the runtime permissions too. A reinstall revokes them, and
        # a rebuild that silently loses the microphone looks exactly like an
        # audio bug -- which cost a debugging round here.
        & $adb install -r -g $apk
        & $adb forward tcp:27183 tcp:27183
        Write-Host "Installed and forwarded tcp:27183."
    }
}
