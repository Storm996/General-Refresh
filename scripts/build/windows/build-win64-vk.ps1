# GeneralsX canonical Windows build - Vulkan core only.
# This is the ONLY supported Windows build entry point.
# It configures and builds build/win64vk/GeneralsMD/Release/GeneralRefresh.exe.
# Any win64 / win32 / vc6 / mingw preset or build directory is retired.

param(
    [string]$Configuration = 'Release',
    [string]$Target = 'z_generals'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$buildDir = Join-Path $repoRoot 'build\win64vk'
$exe = Join-Path $buildDir "GeneralsMD\$Configuration\GeneralRefresh.exe"

# Locate Visual Studio and its CMake/Ninja (the build tree was configured with this toolchain).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsRoot) {
    $fallback = 'D:\Program Files\VS'
    if (Test-Path -LiteralPath (Join-Path $fallback 'VC\Auxiliary\Build\vcvars64.bat')) {
        $vsRoot = $fallback
    }
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

$commands = @(
    "call `"$vcvars`" >nul",
    "`"$cmake`" --preset win64vk $configureArgs",
    "`"$cmake`" --build `"$buildDir`" --config $Configuration --target $Target"
) -join ' && '

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
