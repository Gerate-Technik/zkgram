<#
One-command full pipeline for zkgram on Windows: from a fresh checkout (or a
partially-set-up machine) to a working, portable zkgram.exe with the Qt GUI.

What this does, in order:
  1. Checks prerequisites (git, cmake, an MSVC compiler, Python) and fails
     with a clear message naming what to install if any are missing - this
     script does not install a compiler or Python for you.
  2. Installs TDLib (via install-tdlib.ps1 - vcpkg-bootstrapped static
     OpenSSL/zlib, TDLib built from source, Release-only). Skipped if
     already installed at -TdInstallDir, unless -Clean is passed.
  3. Installs Qt6 (MSVC, prebuilt binaries via aqtinstall - NOT built from
     source through vcpkg, which drags in a full ffmpeg build and dozens of
     other dependencies for ~an hour; see TODO.md, "переключился на
     aqtinstall"). Skipped if already installed at -QtDir/-QtVersion.
  4. Configures and builds zkgram itself (CMake + MSBuild) against the above.
  5. Runs windeployqt to place the Qt runtime DLLs next to zkgram.exe.

The finished client is build\Release\zkgram.exe (no separate packaging/dist
step - this used to also copy everything into a portable dist\ folder plus
bundle CryptoLayer's Python source for running on a machine without this
whole monorepo checked out; removed per explicit request, the build output
now lives only in build\Release\, same place -Config puts it).

Usage:
  .\build.ps1
  .\build.ps1 -Clean          # wipe and rebuild TDLib + zkgram from scratch
  .\build.ps1 -SkipTdlib      # already have TDLib installed, just rebuild zkgram+Qt step
  .\build.ps1 -SkipQt         # already have Qt installed, just rebuild zkgram

This is the ONLY build script in this repo (see this script's own step 4
comment for why the CMake configure+build step used to live in a separate
build-cmake.ps1 - that file is gone, folded in here, per project convention:
one build entry point, not several).

Re-running is safe and incremental: each step is skipped if its output
already exists (TDLib install, Qt install), except the zkgram build itself
and windeployqt, which always re-run (cheap, and must pick up source
changes). Pass -Clean to force a full TDLib rebuild from scratch too.

Total time on a machine with nothing pre-installed: roughly 30-60 minutes,
almost all of it building TDLib from source (step 2) - Qt (step 3) is a
prebuilt download, a few minutes.
#>

param(
    [string]$VcpkgDir = "C:\vcpkg",
    [string]$TdSrcDir = "C:\tdlib-src",
    [string]$TdBuildDir = "C:\tdlib-build",
    [string]$TdInstallDir = "C:\tdlib-install",
    [string]$QtDir = "C:\Qt",
    [string]$QtVersion = "6.9.3",
    [switch]$SkipTdlib,
    [switch]$SkipQt,
    [switch]$Clean,
    # Step 4 (CMake configure+build) options - formerly build-cmake.ps1's own
    # parameters, folded in here.
    [string]$Config = "Release",
    [string]$PythonExe = "",
    [string]$VcpkgBinDir = ""
)

# NOT "Stop": with $ErrorActionPreference = "Stop", PowerShell 5.1 treats
# ANY stderr line from a native exe as a terminating error, even harmless
# warnings (cmake's own deprecation notices, for one) - every external
# command below is checked via $LASTEXITCODE explicitly instead, which is
# what actually reflects success/failure.
$ErrorActionPreference = "Continue"

if ($env:OS -ne "Windows_NT") {
    Write-Error "build-all.ps1 supports Windows only."
    exit 1
}

function Test-CommandExists([string]$name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

Write-Host "=== 1. Checking prerequisites ===" -ForegroundColor Cyan

$missing = @()
if (-not (Test-CommandExists "git")) { $missing += "git (https://git-scm.com/download/win)" }
if (-not (Test-CommandExists "cmake")) { $missing += "CMake (https://cmake.org/download/)" }
if (-not (Test-CommandExists "python")) { $missing += "Python 3.10+ (https://www.python.org/downloads/) - must be on PATH as 'python'" }
# cl.exe (the MSVC compiler) is only on PATH inside a "Developer PowerShell
# for VS" / after vcvarsall.bat - checking for it directly on a plain
# PowerShell prompt would false-negative on a perfectly fine install, so
# check for the Visual Studio installer's own inventory tool instead
# (vswhere, ships with every VS/Build Tools install since 2017).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$hasMsvc = $false
if (Test-Path $vswhere) {
    $vsInstalls = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $hasMsvc = [bool]$vsInstalls
}
if (-not $hasMsvc) {
    $missing += "Visual Studio 2022 Build Tools with the 'Desktop development with C++' workload (https://visualstudio.microsoft.com/downloads/, under 'Tools for Visual Studio')"
}

if ($missing.Count -gt 0) {
    Write-Host "Missing prerequisites - install these first, then re-run this script:" -ForegroundColor Red
    foreach ($item in $missing) { Write-Host "  - $item" -ForegroundColor Red }
    exit 1
}
Write-Host "All prerequisites found."

Write-Host ""
Write-Host "=== 2. TDLib ===" -ForegroundColor Cyan

$tdlibAlreadyInstalled = Test-Path "$TdInstallDir\include\td"

if ($SkipTdlib) {
    Write-Host "-SkipTdlib passed, assuming TDLib is already installed at $TdInstallDir."
    if (-not $tdlibAlreadyInstalled) {
        Write-Error "-SkipTdlib passed but $TdInstallDir\include\td does not exist. Remove -SkipTdlib or fix -TdInstallDir."
        exit 1
    }
} elseif ($tdlibAlreadyInstalled -and -not $Clean) {
    # Same "already installed, skip the heavy step" shortcut -SkipTdlib
    # gives explicitly, but automatic - install-tdlib.ps1 is no longer
    # guaranteed to exist in every checkout (removed on some branches), so
    # this build must not hard-depend on it when there is nothing for it
    # to actually do.
    Write-Host "TDLib already installed at $TdInstallDir, skipping install-tdlib.ps1."
} else {
    if (-not (Test-Path "$PSScriptRoot\install-tdlib.ps1")) {
        Write-Error "TDLib is not installed at $TdInstallDir and install-tdlib.ps1 is missing from this checkout, so it cannot be installed automatically. Either restore install-tdlib.ps1 or install TDLib manually and pass -SkipTdlib."
        exit 1
    }
    $tdlibArgs = @{
        VcpkgDir = $VcpkgDir
        TdSrcDir = $TdSrcDir
        TdBuildDir = $TdBuildDir
        TdInstallDir = $TdInstallDir
        SkipIfInstalled = $true
    }
    if ($Clean) { $tdlibArgs["Clean"] = $true }
    & "$PSScriptRoot\install-tdlib.ps1" @tdlibArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$vcpkgInstalled = Join-Path $VcpkgDir "installed\x64-windows-static-md-release"

Write-Host ""
Write-Host "=== 3. Qt6 ($QtVersion, MSVC, prebuilt via aqtinstall) ===" -ForegroundColor Cyan

$qtInstallPath = Join-Path $QtDir "$QtVersion\msvc2022_64"
if ($SkipQt -or (Test-Path "$qtInstallPath\bin\windeployqt.exe")) {
    Write-Host "Qt6 already installed at $qtInstallPath, skipping."
} else {
    Write-Host "Installing aqtinstall (pip) ..."
    python -m pip install --quiet aqtinstall
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "Downloading Qt $QtVersion win64_msvc2022_64 + qtmultimedia into $QtDir ..."
    python -m aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -m qtmultimedia -O $QtDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path "$qtInstallPath\bin\windeployqt.exe")) {
    Write-Error "windeployqt.exe not found at $qtInstallPath\bin after the Qt install step - something went wrong."
    exit 1
}

Write-Host ""
Write-Host "=== 4. Configuring and building zkgram (Qt UI + vendored lib_ui) ===" -ForegroundColor Cyan

# -Clean here also covers "the CMake build/ dir went stale" (e.g. after any
# changes to TDLib/Qt itself under -TdInstallDir/-QtDir - CMake does not
# watch those for changes the way it watches this repo's own source files),
# not just "rebuild TDLib from scratch" (step 2 above).
if ($Clean -and (Test-Path "$PSScriptRoot\build")) {
    Write-Host "-Clean passed: removing build/ before reconfiguring ..."
    Remove-Item -Recurse -Force "$PSScriptRoot\build"
}

# Resolve cmake.exe explicitly, not bare "cmake": on a machine with both an
# official CMake and MSYS2's C:\msys64\ucrt64\bin\cmake.exe on PATH, the
# MSYS2 one resolves find_package(OpenSSL)/find_package(Td)/find_package(Qt6)
# against its own MinGW libraries instead of the MSVC ones zkgram needs,
# breaking the build silently (see TODO.md for how this was found while
# building TDLib).
$cmakeCandidates = Get-Command cmake -All -ErrorAction SilentlyContinue
if (-not $cmakeCandidates) {
    Write-Error "cmake not found on PATH. Install it first."
    exit 1
}
$preferredCmake = $cmakeCandidates | Where-Object { $_.Source -notmatch "(?i)msys64|mingw" } | Select-Object -First 1
$cmakeExe = if ($preferredCmake) { $preferredCmake.Source } else { $cmakeCandidates[0].Source }
if (-not $preferredCmake) {
    Write-Host "Warning: only found MSYS2/MinGW cmake.exe candidates on PATH ($cmakeExe). This is known to break dependency detection for this project." -ForegroundColor Yellow
}

$cmakeArgs = @("-S", "$PSScriptRoot", "-B", "$PSScriptRoot\build", "-DPYBIND11_FINDPYTHON=ON")
if ($PythonExe -ne "") { $cmakeArgs += "-DPython3_EXECUTABLE=$PythonExe" }
# third_party/desktop-app's own external_jpeg/external_lz4/external_zlib come
# from vcpkg's x64-windows (dynamic) triplet specifically, not the
# x64-windows-static-md-release one TDLib/OpenSSL use above - see the comment
# above zkgram_allow_nonsystem32_deps in third_party/desktop-app/CMakeLists.txt.
$cmakeArgs += "-DCMAKE_DISABLE_FIND_PACKAGE_xxHash=ON"
$cmakeArgs += "-DCMAKE_DISABLE_FIND_PACKAGE_range-v3=ON"
$cmakeArgs += "-DCMAKE_DISABLE_FIND_PACKAGE_tl-expected=ON"
$cmakeArgs += "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON"

$prefixPaths = @("$TdInstallDir;$vcpkgInstalled", $qtInstallPath)
$cmakeArgs += "-DCMAKE_PREFIX_PATH=$($prefixPaths -join ';')"

& $cmakeExe @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# /m:1 (single-threaded MSBuild): this machine has 5.9GB RAM total, and
# parallel MSBuild (the default) has repeatedly failed here with
# "C1060: compiler is out of heap space" once lib_ui's ~446 files are in the
# build - see zkgram/.claude/UI.md section 13c.
& $cmakeExe --build "$PSScriptRoot\build" --config $Config -- /m:1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# TDLib is now built against OpenSSL/zlib from vcpkg's x64-windows-static-md
# triplet (static .lib, not DLLs), so the exe has no OpenSSL/zlib runtime
# dependency and no DLLs need to sit next to it - see TODO.md for how this
# replaced the earlier x64-windows (dynamic) triplet. -VcpkgBinDir is only
# for the rare case of building against the dynamic triplet again; empty by
# default means no copy step runs.
if ($VcpkgBinDir -ne "") {
    $stepOutDir = Join-Path "$PSScriptRoot\build" $Config
    if ((Test-Path $VcpkgBinDir) -and (Test-Path $stepOutDir)) {
        Get-ChildItem -Path $VcpkgBinDir -Filter "*.dll" | ForEach-Object {
            Copy-Item -Path $_.FullName -Destination $stepOutDir -Force
        }
    } else {
        Write-Host "Warning: -VcpkgBinDir '$VcpkgBinDir' not found, did not copy DLLs." -ForegroundColor Yellow
    }
}

$exePath = Join-Path $PSScriptRoot "build\Release\zkgram.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "Build succeeded but $exePath does not exist - unexpected."
    exit 1
}

Write-Host ""
Write-Host "=== 5. Deploying Qt runtime DLLs ===" -ForegroundColor Cyan

& "$qtInstallPath\bin\windeployqt.exe" $exePath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Green
Write-Host "Build ready at: $exePath"
