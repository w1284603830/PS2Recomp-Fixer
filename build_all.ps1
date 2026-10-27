param(
    [string]$BuildDir = '',          # default: <this folder>\build
    [ValidateSet('all', 'kzvu', 'gs', 'ps2recomp')][string]$Part = 'all',
    [switch]$NoTests                 # skip running ps2x_tests.exe at the end
)
# Builds everything from third_party\ in this folder (fetch_deps.ps1 first in a fresh clone; FFmpeg's pinned
# prebuilt package is downloaded by CMake unless third_party\_downloads has it). Order: kzvu.lib (PCSX2 microVU,
# linked into the runtime) -> pcsx2gs.dll (PCSX2 hardware GS) -> PS2Recomp (ps2_recomp, ps2_analyzer, ps2_runtime +
# ps2EntryRunner, the test programs), then the tests. Needs Visual Studio 2022 (MSVC x64) and CMake 3.24+. Run in
# PowerShell, not Git Bash (Git Bash rewrites /DWIN32-style flags into paths).
#   powershell -NoProfile -ExecutionPolicy Bypass -File build_all.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if ($BuildDir -eq '') { $BuildDir = Join-Path $root 'build' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$src = Join-Path $root 'third_party\src'
$dl = Join-Path $root 'third_party\_downloads'
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    foreach ($c in 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
                   'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe') {
        if (Test-Path $c) { $cmake = $c; break }
    }
}
if (-not $cmake) { throw 'cmake not found (install Visual Studio 2022 with C++ CMake tools)' }
$gen = @('-G', 'Visual Studio 17 2022', '-A', 'x64')

function Step([string]$name, [string[]]$configure, [string[]]$targets) {
    $log = Join-Path $BuildDir "$name.log"
    "== $name -> $log"
    $ErrorActionPreference = 'Continue'   # a native command's stderr line must not stop the script
    & $cmake @configure 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
    if ($LASTEXITCODE -ne 0) { $ErrorActionPreference = 'Stop'; Get-Content $log -Tail 30; throw "$name configure failed" }
    foreach ($t in $targets) {
        & $cmake --build (Join-Path $BuildDir $name) --config Release --target $t --parallel 2>&1 |
            ForEach-Object { "$_" } | Out-File -Append -Encoding utf8 $log
        if ($LASTEXITCODE -ne 0) {
            $ErrorActionPreference = 'Stop'
            Get-Content $log | Select-String -Pattern ': (fatal )?error ' | Select-Object -First 20 | ForEach-Object { $_.Line }
            throw "$name target $t failed"
        }
    }
    $ErrorActionPreference = 'Stop'
    "   ok"
}

$missing = @('raylib', 'SDL2-2.30.9', 'elfio', 'fmt', 'rabbitizer', 'toml11', 'libdwarf', 'nlohmann_json') |
    Where-Object { -not (Test-Path (Join-Path $src $_)) }
if (-not (Test-Path (Join-Path $dl 'pcsx2_646df006a4\pcsx2'))) { $missing += 'pcsx2_646df006a4' }
if ($missing) { throw "missing dependencies ($($missing -join ', ')): run fetch_deps.ps1 first" }

New-Item -ItemType Directory -Force $BuildDir | Out-Null
$kzvuLib = Join-Path $BuildDir 'kzvu\Release\kzvu.lib'

if ($Part -in 'all', 'kzvu') {
    Step 'kzvu' (@('-S', (Join-Path $src 'kzvu'), '-B', (Join-Path $BuildDir 'kzvu')) + $gen +
        @("-DKZVU_PCSX2_DIR=$(Join-Path $dl 'pcsx2_646df006a4')", '-DKZVU_BUILD_TEST=OFF')) @('kzvu')
}
if ($Part -in 'all', 'gs') {
    Step 'pcsx2gs' (@('-S', (Join-Path $src 'pcsx2gs\pcsx2-gs'), '-B', (Join-Path $BuildDir 'pcsx2gs')) + $gen) @('pcsx2gs')
}
if ($Part -in 'all', 'ps2recomp') {
    $fc = @('RAYLIB=raylib', 'ELFIO=elfio', 'TOML11=toml11', 'FMT=fmt', 'LIBDWARF=libdwarf', 'RABBITIZER=rabbitizer',
            'NLOHMANN_JSON=nlohmann_json') | ForEach-Object { $k, $v = $_ -split '='; "-DFETCHCONTENT_SOURCE_DIR_$k=$(Join-Path $src $v)" }
    # FFmpeg: a local copy of the pinned prebuilt zip when there is one, else the runtime's CMake downloads it.
    $ffmpeg = Join-Path $dl 'ffmpeg-n7.1-241205-lgpl-amd64-shared.zip'
    if (Test-Path $ffmpeg) { $fc += "-DPS2X_FFMPEG_ARCHIVE=$ffmpeg" }
    $opts = @('-DPS2X_BUILD_RECOMP=ON', '-DPS2X_BUILD_ANALYZER=ON', '-DPS2X_BUILD_RUNTIME=ON', '-DPS2X_BUILD_TEST=ON',
              '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_ENABLE_FFMPEG=ON', '-DPS2X_ENABLE_DEBUG_UI=OFF', '-DPS2X_ENABLE_SCCACHE=OFF',
              '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
              '-DPS2X_IOP_BUILD_TESTS=ON', '-DBUILD_TESTING=ON',
              "-DDOD_KZVU_LIB=$kzvuLib",
              '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /MP /bigobj /utf-8', '-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /MP /utf-8')
    Step 'ps2recomp' (@('-S', (Join-Path $src 'PS2Recomp'), '-B', (Join-Path $BuildDir 'ps2recomp')) + $gen + $opts + $fc) @('ALL_BUILD')
    if (-not $NoTests) {
        # 25 of ps2x_tests' 446 are known to fail: they expect upstream's behaviour where this base follows the
        # hardware / PCSX2 (BUILD_VERIFICATION.md). The other suites (IOP, GS cache) must all pass.
        $ErrorActionPreference = 'Continue'
        $exe = Join-Path $BuildDir 'ps2recomp\ps2xTest\Release\ps2x_tests.exe'
        "== ps2x_tests -> ps2x_tests.log"
        Push-Location (Split-Path $exe)
        try { & $exe 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 (Join-Path $BuildDir 'ps2x_tests.log'); $code = $LASTEXITCODE }
        finally { Pop-Location }
        Get-Content (Join-Path $BuildDir 'ps2x_tests.log') -Tail 4
        "   exit $code (failed tests)"
        # The top-level CMakeLists has no enable_testing(), so CTest sees nothing: run the suites directly.
        $failed = 0
        "== IOP tests"
        foreach ($e in Get-ChildItem (Join-Path $BuildDir 'ps2recomp\ps2xIOP\Release') -Filter '*_tests.exe') {
            Push-Location $e.DirectoryName
            try { $out = & $e.FullName 2>&1 | ForEach-Object { "$_" }; $code = $LASTEXITCODE } finally { Pop-Location }
            "   {0}: exit {1} | {2}" -f $e.Name, $code, ($out | Select-Object -Last 1)
            if ($code -ne 0) { $failed++ }
        }
        "== GS cache tests (one process per test name, as gs_cache\CMakeLists.txt registers them)"
        $suites = [regex]::Matches((Get-Content -Raw (Join-Path $src 'PS2Recomp\ps2xTest\gs_cache\CMakeLists.txt')),
                                   'add_gs_cache_suite\((\S+)\s+\S+\s+\S+\s+([^)]*)\)')
        $n = 0; $bad = 0
        foreach ($s in $suites) {
            $exe = Join-Path $BuildDir "ps2recomp\ps2xTest\gs_cache\Release\$($s.Groups[1].Value).exe"
            foreach ($t in ($s.Groups[2].Value -split '\s+' | Where-Object { $_ })) {
                & $exe $t *> $null; $n++
                if ($LASTEXITCODE -ne 0) { $bad++; "   FAIL $($s.Groups[1].Value) $t" }
            }
        }
        "   {0}/{1} passed" -f ($n - $bad), $n
        if ($bad) { $failed++ }
        $ErrorActionPreference = 'Stop'
        if ($failed) { throw "$failed test suite(s) failed" }
    }
}
