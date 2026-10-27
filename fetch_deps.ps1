param(
    [switch]$Force,                 # download and extract again even when a folder already exists
    [switch]$AllowHashMismatch      # accept a GitHub archive whose SHA256 differs from the pinned one (warns)
)
# Downloads the unmodified, version-pinned dependencies that the git repository does not carry (see .gitignore and
# THIRD_PARTY.md) into third_party\src\<name>, the same layout build_all.ps1 and the sources expect:
#   raylib 5.5, ELFIO Release_3.12, toml11 v4.4.0, fmt 12.1.0, libdwarf v2.2.0, rabbitizer 1.14.3,
#   nlohmann/json v3.11.3, SDL2 2.30.9 (VC development package), and PCSX2 at commit 646df006a4 (kzvu builds from it).
# Each zip is checked against its pinned SHA256; PCSX2 is fetched by full commit hash with git, which verifies it.
# FFmpeg is not fetched here: the runtime's CMake downloads its pinned prebuilt package itself.
#   powershell -NoProfile -ExecutionPolicy Bypass -File fetch_deps.ps1
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar in Windows PowerShell
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = $PSScriptRoot
$src = Join-Path $root 'third_party\src'
$dl = Join-Path $root 'third_party\_downloads'
New-Item -ItemType Directory -Force $src, $dl | Out-Null

$zips = @(
    @{ Name = 'raylib'; File = 'raylib-5.5.zip'; Url = 'https://github.com/raysan5/raylib/archive/refs/tags/5.5.zip'
       Sha = '00e8a7c85f7a5221e56ee8e4e1c6642b773eae73d29379a91c8ac5149e803f42' },
    @{ Name = 'elfio'; File = 'ELFIO-Release_3.12.zip'; Url = 'https://github.com/serge1/ELFIO/archive/refs/tags/Release_3.12.zip'
       Sha = '07248db427d1962125cec75e8e79bfde7c7c1c0d0e5656d2d847709aebbfaa39' },
    @{ Name = 'toml11'; File = 'toml11-v4.4.0.zip'; Url = 'https://github.com/ToruNiina/toml11/archive/refs/tags/v4.4.0.zip'
       Sha = '075dd130a9f517c3eaed502ece6a2c1088b1d0dd4aa79882fd0ca7609325ffdc' },
    @{ Name = 'fmt'; File = 'fmt-12.1.0.zip'; Url = 'https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.zip'
       Sha = 'e6a2b43c6ec4779fb53b15b705f7c20b4eb9d5ae35784d610ac6daec30737019' },
    @{ Name = 'libdwarf'; File = 'libdwarf-code-v2.2.0.zip'; Url = 'https://github.com/davea42/libdwarf-code/archive/refs/tags/v2.2.0.zip'
       Sha = '1a015b73b83036dd2ba234c54929f9f773200328df12a0b64d9e01d8c63d4c50' },
    @{ Name = 'rabbitizer'; File = 'rabbitizer-1.14.3.zip'; Url = 'https://github.com/Decompollaborate/rabbitizer/archive/refs/tags/1.14.3.zip'
       Sha = 'a3ac4b5071a3673510515aa29833162b506c1ef0235a15cb614a02fe2f629892' },
    @{ Name = 'nlohmann_json'; File = 'json-v3.11.3.zip'; Url = 'https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip'
       Sha = '04022b05d806eb5ff73023c280b68697d12b93e1b7267a0b22a1a39ec7578069' },
    @{ Name = 'SDL2-2.30.9'; File = 'SDL2-devel-2.30.9-VC.zip'; Url = 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-VC.zip'
       Sha = '8c91d91e5bcb997d062ec2b553c53832ebf95654d4aa35e8c02a954d4ce752ae' }
)

# Extract a zip into $dest, dropping the archive's single top folder (as tools\extract_archives.py does).
function Expand-Stripped([string]$zip, [string]$dest) {
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $top = ($archive.Entries[0].FullName -split '/')[0] + '/'
        $full = [IO.Path]::GetFullPath($dest).TrimEnd('\') + '\'
        foreach ($e in $archive.Entries) {
            if (-not $e.FullName.StartsWith($top)) { throw "unexpected layout in ${zip}: $($e.FullName)" }
            $rel = $e.FullName.Substring($top.Length)
            if ($rel -eq '') { continue }
            $target = [IO.Path]::GetFullPath((Join-Path $dest $rel))
            if (-not $target.StartsWith($full)) { throw "path escapes the destination: $($e.FullName)" }
            if ($e.FullName.EndsWith('/')) { New-Item -ItemType Directory -Force $target | Out-Null; continue }
            New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
        }
    } finally { $archive.Dispose() }
}

foreach ($z in $zips) {
    $dest = Join-Path $src $z.Name
    if ((Test-Path $dest) -and -not $Force) { "{0,-14} present, skipped" -f $z.Name; continue }
    $file = Join-Path $dl $z.File
    if (-not (Test-Path $file)) {
        "{0,-14} downloading {1}" -f $z.Name, $z.Url
        Invoke-WebRequest -UseBasicParsing -Uri $z.Url -OutFile $file
    }
    $sha = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLower()
    if ($sha -ne $z.Sha) {
        if (-not $AllowHashMismatch) { throw "$($z.File): SHA256 $sha, pinned $($z.Sha) (rerun with -AllowHashMismatch to accept)" }
        Write-Warning "$($z.File): SHA256 $sha differs from the pinned $($z.Sha)"
    }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Expand-Stripped $file $dest
    "{0,-14} ok ({1})" -f $z.Name, $sha.Substring(0, 16)
}

# PCSX2 for kzvu (third_party\src\kzvu\README.md): a shallow fetch of exactly this commit.
$pcsx2Commit = '646df006a4688063e56c0826e7ea4b8bb0c112c4'
$pcsx2 = Join-Path $dl 'pcsx2_646df006a4'
if ((Test-Path (Join-Path $pcsx2 'pcsx2')) -and -not $Force) { 'pcsx2          present, skipped' }
else {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git is needed to fetch PCSX2 (https://git-scm.com)' }
    if (Test-Path $pcsx2) { Remove-Item -Recurse -Force $pcsx2 }
    'pcsx2          fetching commit ' + $pcsx2Commit
    $ErrorActionPreference = 'Continue'   # git reports progress on stderr
    git init -q $pcsx2
    git -C $pcsx2 fetch -q --depth 1 https://github.com/PCSX2/pcsx2.git $pcsx2Commit 2>&1 | ForEach-Object { "  $_" }
    git -C $pcsx2 -c advice.detachedHead=false checkout -q FETCH_HEAD 2>&1 | ForEach-Object { "  $_" }
    $head = (git -C $pcsx2 rev-parse HEAD).Trim()
    $ErrorActionPreference = 'Stop'
    if ($head -ne $pcsx2Commit) { throw "PCSX2 checkout is $head, expected $pcsx2Commit" }
    'pcsx2          ok'
}
'done; next: powershell -NoProfile -ExecutionPolicy Bypass -File build_all.ps1'
