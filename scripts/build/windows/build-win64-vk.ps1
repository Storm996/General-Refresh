# GeneralsX canonical Windows build - Vulkan core only.
# This is the ONLY supported Windows build entry point.
# It configures and builds build/win64vk/GeneralsMD/Release/GeneralRefresh.exe.
# Any win64 / win32 / vc6 / mingw preset or build directory is retired.

param(
    [string]$Configuration = 'Release',
    # GeneralsX @feature 24/10/2026 P34 F6: the base game (Generals) now has a build entry too, so the
    # lighting backport can be compiled and checked instead of being "another tree nobody builds".
    # ZeroHour keeps the canonical build/win64vk directory; Generals gets its own cache.
    [ValidateSet('ZeroHour', 'Generals')][string]$Game = 'ZeroHour',
    [string]$Target = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
if ($Game -eq 'Generals') {
    $buildDir = Join-Path $repoRoot 'build\win64vk-generals'
    if (-not $Target) { $Target = 'g_generals' }
    $exe = Join-Path $buildDir "Generals\$Configuration\GeneralsX.exe"
} else {
    $buildDir = Join-Path $repoRoot 'build\win64vk'
    if (-not $Target) { $Target = 'z_generals' }
    $exe = Join-Path $buildDir "GeneralsMD\$Configuration\GeneralRefresh.exe"
}

# Locate Visual Studio and its CMake/Ninja (the build tree was configured with this toolchain).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
# GeneralsX @bugfix 23/10/2026: vswhere returns the installation's real path. On this machine that is
# D:\软件\VS while the ASCII junction D:\Program Files\VS points at the same install. cmd.exe /c then
# calls vcvars64.bat through the OEM code page and mangles the non-ASCII path, so the canonical build
# failed with a "Missing vcvars64.bat: D:\???VS" message even though the file exists. Prefer an ASCII
# installation path and fall back to the explicit junction before giving up.
if ($vsRoot -and ($vsRoot -match '[^\x00-\x7F]')) { $vsRoot = $null }
if (-not $vsRoot) {
    $fallback = 'D:\Program Files\VS'
    if (Test-Path -LiteralPath (Join-Path $fallback 'VC\Auxiliary\Build\vcvars64.bat')) {
        $vsRoot = $fallback
    }
}
if ($vsRoot -and -not (Test-Path -LiteralPath (Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'))) {
    $vsRoot = $null
}
if (-not $vsRoot) {
    throw 'Visual Studio with the x64 C++ toolchain was not found. Install VS 2022 C++ tools first.'
}

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "Missing vcvars64.bat: $vcvars" }
if (-not (Test-Path -LiteralPath $cmake))  { throw "Missing cmake.exe: $cmake" }

# Offline dependency cache. The old build/win64 tree used to carry these sources; they now live in
# deps/ so the canonical build never reaches into a retired build directory.
$deps = Join-Path $repoRoot 'deps'
$depMap = [ordered]@{
    DX8        = 'dx8'
    GAMEMATH   = 'gamemath'
    GAMESPY    = 'gamespy'
    STB        = 'stb'
    ZLIB       = 'zlib'
    LZHL       = 'lzhl\CompLibHeader'
    MINIAUDIO  = 'miniaudio'
    GLM_FETCH  = 'glm'
    GLI_FETCH  = 'gli'
}
$depArgs = @()
foreach ($key in $depMap.Keys) {
    $path = Join-Path $deps $depMap[$key]
    if (Test-Path -LiteralPath $path) {
        $depArgs += "-DFETCHCONTENT_SOURCE_DIR_$key=$($path -replace '\\','/')"
    }
}
$configureArgs = ''
if ($depArgs.Count -eq $depMap.Count) {
    $configureArgs = '-DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_UPDATES_DISCONNECTED=ON'
}
if ($depArgs.Count -gt 0) {
    $configureArgs = ($configureArgs + ' ' + ($depArgs -join ' ')).Trim()
}

Write-Host '=== GeneralsX Windows build (Vulkan core, canonical) ===' -ForegroundColor Cyan
Write-Host "Repo:    $repoRoot"
Write-Host "Build:   $buildDir"
Write-Host "Config:  $Configuration"
Write-Host "Target:  $Target"
Write-Host "Deps:    $($depArgs.Count)/$($depMap.Count) from $deps"

# GeneralsX @bugfix 24/10/2026 DEPSCAN: cl.exe writes its /showIncludes lines in the console output
# code page. On this Chinese-locale toolchain that was GBK while CMake stores the detected prefix as
# UTF-8, so ninja's "deps = msvc" never matched a single include line and recorded NO header
# dependencies: editing a header rebuilt only the files whose .cpp changed. The stale objects then
# disagreed with the rebuilt ones about class layouts (DX8VertexBufferClass grew a member), and the
# game corrupted its own memory pool on the first frame that destroyed a vertex buffer.
# Running the whole configure+build under code page 65001 makes cl emit UTF-8, which is exactly the
# encoding CMake reads and writes, so dependency tracking works from then on.
$commands = @(
    "chcp 65001 >nul",
    "call `"$vcvars`" >nul",
    "`"$cmake`" --preset win64vk $configureArgs"
) -join ' && '
if ($Game -eq 'Generals') {
    # Reuse the canonical preset and override only the game-selection knobs plus the binary directory.
    $commands = @(
        "chcp 65001 >nul",
        "call `"$vcvars`" >nul",
        "`"$cmake`" --preset win64vk -B `"$buildDir`" -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=OFF $configureArgs"
    ) -join ' && '
}
$commands += " && `"$cmake`" --build `"$buildDir`" --config $Configuration --target $Target"

& cmd.exe /c $commands
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

# Hard guard: a build without the Vulkan core is NOT a valid game build any more.
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) { throw "Missing CMake cache: $cache" }
$cacheText = Get-Content -LiteralPath $cache -Raw
if ($cacheText -notmatch 'SAGE_USE_VULKAN_CORE:BOOL=ON') {
    throw 'REFUSING BUILD: SAGE_USE_VULKAN_CORE is not ON. The game is pure Vulkan; do not use win64/win32/vc6 presets.'
}
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Build reported success but the expected exe is missing: $exe"
}

$item = Get-Item -LiteralPath $exe
Write-Host ''
Write-Host 'BUILD OK (Vulkan core)' -ForegroundColor Green
Write-Host ("EXE: {0}" -f $item.FullName)
Write-Host ("SIZE: {0} bytes" -f $item.Length)
Write-Host ("TIME: {0}" -f $item.LastWriteTime)
