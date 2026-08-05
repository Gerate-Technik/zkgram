param(
    [ValidateSet("console", "qt")]
    [string]$Ui = "console",
    [string]$Config = "Release",
    [string]$PythonExe = "",
    [string]$TdPrefix = "",
    [string]$QtPrefix = "",
    [string]$VcpkgBinDir = "",
    [switch]$Clean
)

if ($env:OS -ne "Windows_NT") {
    Write-Error "zkgram build.ps1 supports Windows only."
    exit 1
}

# Re-running this script is safe and incremental by default: CMake only
# reconfigures what changed (fast) and MSBuild only recompiles .cpp files
# whose content or included headers changed - editing one file and
# re-running does not rebuild the world. -Clean removes the build/ directory
# first, for the rare cases incremental state genuinely goes stale (e.g.
# switching -Ui console/qt on an existing build/, or after any changes to
# TDLib/Qt itself under C:\tdlib-install or a Qt prefix - CMake does not
# watch those for changes the way it watches this repo's own source files).
if ($Clean -and (Test-Path "build")) {
    Write-Host "-Clean passed: removing build/ before reconfiguring ..."
    Remove-Item -Recurse -Force "build"
}

# Resolve cmake.exe explicitly, not bare "cmake": on a machine with both an
# official CMake and MSYS2's C:\msys64\ucrt64\bin\cmake.exe on PATH, the
# MSYS2 one resolves find_package(OpenSSL)/find_package(Td)/find_package(Qt6)
# against its own MinGW libraries instead of the MSVC ones zkgram needs,
# breaking the build silently (see install-tdlib.ps1/TODO.md for how this
# was found while building TDLib).
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

$cmakeArgs = @("-S", ".", "-B", "build", "-DPYBIND11_FINDPYTHON=ON", "-DZKGRAM_UI=$Ui")
if ($PythonExe -ne "") { $cmakeArgs += "-DPython3_EXECUTABLE=$PythonExe" }

$prefixPaths = @()
if ($TdPrefix -ne "") { $prefixPaths += $TdPrefix }
if ($QtPrefix -ne "") { $prefixPaths += $QtPrefix }
if ($prefixPaths.Count -gt 0) { $cmakeArgs += "-DCMAKE_PREFIX_PATH=$($prefixPaths -join ';')" }

& $cmakeExe @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmakeExe --build build --config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# TDLib is now built against OpenSSL/zlib from vcpkg's x64-windows-static-md
# triplet (static .lib, not DLLs), so the exe has no OpenSSL/zlib runtime
# dependency and no DLLs need to sit next to it - see TODO.md for how this
# replaced the earlier x64-windows (dynamic) triplet. -VcpkgBinDir is only
# for the rare case of building against the dynamic triplet again; empty by
# default means no copy step runs.
if ($VcpkgBinDir -ne "") {
    $outDir = Join-Path "build" $Config
    if ((Test-Path $VcpkgBinDir) -and (Test-Path $outDir)) {
        Get-ChildItem -Path $VcpkgBinDir -Filter "*.dll" | ForEach-Object {
            Copy-Item -Path $_.FullName -Destination $outDir -Force
        }
    } else {
        Write-Host "Warning: -VcpkgBinDir '$VcpkgBinDir' not found, did not copy DLLs." -ForegroundColor Yellow
    }
}

Write-Host "Build finished. Run: build\$Config\zkgram.exe"
