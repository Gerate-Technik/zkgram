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
  4. Builds zkgram itself (via build-cmake.ps1 -Ui qt) against the above.
  5. Runs windeployqt to place the Qt runtime DLLs next to zkgram.exe.
  6. Copies everything a user needs to run the app - zkgram.exe, the Qt
     DLLs windeployqt placed, and TDLib's OpenSSL/zlib being statically
     linked means nothing else is needed - into a clean -OutDir. That whole
     folder (not just zkgram.exe alone) is what must be copied/zipped/moved
     to run it anywhere else; Qt itself is still dynamically linked (see
     TODO.md, "план: портативность exe" for why full static Qt linking is a
     separate, much larger undertaking not done here).

Usage:
  .\build.ps1
  .\build.ps1 -OutDir D:\zkgram-dist
  .\build.ps1 -Clean          # wipe and rebuild TDLib + zkgram from scratch
  .\build.ps1 -SkipTdlib      # already have TDLib installed, just rebuild zkgram+Qt step
  .\build.ps1 -SkipQt         # already have Qt installed, just rebuild zkgram

For just the CMake configure+build step on its own (no TDLib/Qt install,
no windeployqt, no dist/ packaging), use build-cmake.ps1 directly - this
script's own step 4 is nothing more than a call to it.

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
    [string]$OutDir = "$PSScriptRoot\dist",
    [switch]$SkipTdlib,
    [switch]$SkipQt,
    [switch]$Clean,
    # See build-cmake.ps1's own -VendorLibUi comment.
    [switch]$VendorLibUi
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
Write-Host "=== 4. Building zkgram (Qt UI) ===" -ForegroundColor Cyan

$buildCmakeArgs = @{
    Ui = "qt"
    TdPrefix = "$TdInstallDir;$vcpkgInstalled"
    QtPrefix = $qtInstallPath
}
if ($VendorLibUi) { $buildCmakeArgs["VendorLibUi"] = $true }
& "$PSScriptRoot\build-cmake.ps1" @buildCmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

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
Write-Host "=== 6. Packaging a portable copy into $OutDir ===" -ForegroundColor Cyan
# The whole build\Release\ folder (exe + every DLL windeployqt placed next
# to it) is what needs to move together - copying zkgram.exe alone and
# running it from somewhere else (Desktop, a USB stick) fails with missing
# Qt6Core.dll and friends, because Qt itself is still linked dynamically
# (only TDLib/OpenSSL/zlib are static, see the file header). This step
# exists specifically so "the output" is one clean folder, not "go find
# build\Release\ and remember every file in it has to come along."

if (Test-Path $OutDir) {
    Remove-Item -Recurse -Force $OutDir
}
New-Item -ItemType Directory -Path $OutDir | Out-Null
# Excludes "data": TDLib's local session/identity database - if present in
# build\Release\ at all, it is leftover from running zkgram.exe directly
# out of that folder during development/testing, never something to ship.
# A real user's own session gets created fresh the first time they run the
# packaged exe.
Copy-Item -Path (Join-Path $PSScriptRoot "build\Release\*") -Destination $OutDir -Recurse -Exclude "*.pdb", "*.lib", "*.exp", "data"
# Copy-Item -Exclude does not reliably skip whole subdirectories by name
# during a -Recurse copy on PowerShell 5.1 (only filters file name
# patterns in some versions) - remove it explicitly as a safety net rather
# than trust -Exclude alone for this one.
$strayDataDir = Join-Path $OutDir "data"
if (Test-Path $strayDataDir) {
    Remove-Item -Recurse -Force $strayDataDir
}

# The exe's own sys.path setup (src/crypto/python_bridge.cpp) needs
# cryptolayer/src, cryptolayer-module-interface, and python/bridge.py to run
# on a machine that does not have this whole monorepo checked out - without
# this, a copy of dist/ on another PC fails at startup with "No module named
# bridge" (Python found, but the CryptoLayer source it needs to import was
# never there). python_bridge.cpp looks for exactly this "python-deps"
# folder next to zkgram.exe and prefers it over the compile-time dev paths
# when present.
Write-Host ""
Write-Host "=== 7. Bundling CryptoLayer Python source into $OutDir\python-deps ===" -ForegroundColor Cyan
$pythonDepsDir = Join-Path $OutDir "python-deps"
New-Item -ItemType Directory -Path (Join-Path $pythonDepsDir "cryptolayer") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pythonDepsDir "python") -Force | Out-Null
Copy-Item -Path (Join-Path $PSScriptRoot "..\cryptolayer\src") -Destination (Join-Path $pythonDepsDir "cryptolayer\src") -Recurse -Force
Copy-Item -Path (Join-Path $PSScriptRoot "..\cryptolayer-module-interface") -Destination (Join-Path $pythonDepsDir "cryptolayer-module-interface") -Recurse -Force
Copy-Item -Path (Join-Path $PSScriptRoot "python\bridge.py") -Destination (Join-Path $pythonDepsDir "python\bridge.py") -Force
# .git/, __pycache__/, and *.egg-info are development artifacts from the
# source checkouts above, not something Python needs to import these
# modules - stripped so the packaged zip does not carry a second copy of
# cryptolayer-module-interface's git history around.
Get-ChildItem -Path $pythonDepsDir -Recurse -Directory -Force | Where-Object {
    $_.Name -eq ".git" -or $_.Name -eq "__pycache__" -or $_.Name -like "*.egg-info"
} | Remove-Item -Recurse -Force

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Green
Write-Host "Portable build ready at: $OutDir"
Write-Host "Run it: $OutDir\zkgram.exe"
Write-Host "To move it to another folder/PC, copy the ENTIRE $OutDir folder, not just zkgram.exe."
