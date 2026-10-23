# GeneralsX canonical Windows deploy - Vulkan core only.
# Copies the win64vk Release exe, the right-side sidebar layout and the x64 DXVK DLLs
# into the retail game root. The previous exe is timestamp-backed up first.

param(
    [string]$GameRoot = 'D:\Games\Generals\Command and Conquer Generals Zero Hour'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$buildDir = Join-Path $repoRoot 'build\win64vk'
$srcExe = Join-Path $buildDir 'GeneralsMD\Release\GeneralRefresh.exe'
$dstExe = Join-Path $GameRoot 'GeneralRefresh.exe'

if (-not (Test-Path -LiteralPath $srcExe)) {
    throw "Canonical Vulkan exe missing: $srcExe. Run scripts\build\windows\build-win64-vk.ps1 first."
}

$cache = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) { throw "Missing CMake cache: $cache" }
if ((Get-Content -LiteralPath $cache -Raw) -notmatch 'SAGE_USE_VULKAN_CORE:BOOL=ON') {
    throw 'REFUSING DEPLOY: this build tree is not the Vulkan-core build.'
}

if (-not (Test-Path -LiteralPath $GameRoot)) {
    throw "Game root not found: $GameRoot"
}

$running = Get-Process -Name 'GeneralRefresh' -ErrorAction SilentlyContinue
if ($running) {
    throw 'GeneralRefresh.exe is running. Close the game before deploying (the exe is file-locked).'
}

if (Test-Path -LiteralPath $dstExe) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $backup = "$dstExe.bak_${stamp}_vk"
    Copy-Item -LiteralPath $dstExe -Destination $backup -Force
    Write-Host "Backup: $backup"
}

Copy-Item -LiteralPath $srcExe -Destination $dstExe -Force
Write-Host "Exe:    $dstExe"

$wndSrc = Join-Path $repoRoot 'dist\Window\RightSidebar.wnd'
if (Test-Path -LiteralPath $wndSrc) {
    $wndDstDir = Join-Path $GameRoot 'Window'
    New-Item -ItemType Directory -Force -Path $wndDstDir | Out-Null
    Copy-Item -LiteralPath $wndSrc -Destination (Join-Path $wndDstDir 'RightSidebar.wnd') -Force
    Write-Host "WND:    $(Join-Path $wndDstDir 'RightSidebar.wnd')"
}

$dxvkSrc = Join-Path $repoRoot 'dist\dxvk-x64'
foreach ($dll in @('d3d8.dll', 'd3d9.dll', 'dxgi.dll')) {
    $src = Join-Path $dxvkSrc $dll
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $GameRoot $dll) -Force
        Write-Host "DXVK:   $dll"
    }
}

$item = Get-Item -LiteralPath $dstExe
Write-Host ''
Write-Host 'DEPLOY OK (Vulkan core)' -ForegroundColor Green
Write-Host ("EXE:  {0}" -f $item.FullName)
Write-Host ("SIZE: {0} bytes" -f $item.Length)
Write-Host ("TIME: {0}" -f $item.LastWriteTime)
