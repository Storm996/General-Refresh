---
applyTo: 'cmake/**,CMakeLists.txt,CMakePresets.json'
---

## Canonical Build Matrix

The Windows game is **pure Vulkan**. There is exactly one supported Windows build.

### Windows (ONLY)
- **`win64vk`** — MSVC x64, Release, `SAGE_USE_VULKAN_CORE=ON`
- Entry point: `build-win64-vk.bat` (wraps `scripts/build/windows/build-win64-vk.ps1`)
- Deploy: `deploy-win64-vk.bat` (wraps `scripts/build/windows/deploy-win64-vk.ps1`)
- Product: `build/win64vk/GeneralsMD/Release/GeneralRefresh.exe`

**Never call or recreate:** `build/win64`, `build/win32`, `build/win32-vcpkg`, or presets
`win64`, `win64-vcpkg`, `win32*`, `vc6*`, `unix`, `mingw-w64-i686*`. They have been deleted.

### Linux
- **`linux64-deploy`** — GCC/Clang x86_64, Release — PRIMARY LINUX
- **`linux64-miniaudio`** — Linux MiniAudio variant

```bash
./scripts/build/linux/docker-configure-linux.sh linux64-deploy
./scripts/build/linux/docker-build-linux-zh.sh linux64-deploy

# or native
cmake --preset linux64-deploy
cmake --build build/linux64-deploy --target z_generals
```

### macOS
- **`macos-vulkan`** — macOS ARM64, RelWithDebInfo — PRIMARY MACOS

```bash
cmake --preset macos-vulkan
cmake --build build/macos-vulkan --target z_generals
```

## Guard Rails

1. Before trusting a Windows exe, check `build/win64vk/CMakeCache.txt` contains
   `SAGE_USE_VULKAN_CORE:BOOL=ON`.
2. If a build path does not contain `win64vk`, it is the wrong build — stop.
3. `scripts/build/windows/build-win64-vk.ps1` hard-fails when the cache is not Vulkan-core.
4. Do not re-add 32-bit or non-Vulkan presets; the user explicitly removed them.

## DXVK Source of Truth (macOS / Windows runtime)

- DXVK fixes must live in `generalsx-dxvk` and be pushed to the fork branch `generalsx-macos-v2.6`.
- macOS build tracks that branch via CMake FetchContent (`UPDATE_DISCONNECTED FALSE`).
- Local mode: `-DSAGE_DXVK_USE_LOCAL_FORK=ON` (disables update/fetch).
- Windows runtime DLLs live in `dist/dxvk-x64/` and are copied by the deploy script.

**Rules**:
1. Never patch DXVK files inside `build/_deps/...`.
2. Do not rely on transient patch scripts.
3. Keep Linux/macOS aligned on DXVK 2.6 branch unless explicitly changed.
4. Validate fix locally → commit/push to fork → CMake consumes from tracked branch.

## Testing Strategy

1. Per-platform smoke tests: launch game, reach main menu, load skirmish map.
2. Replay compatibility: build with `RTS_BUILD_OPTION_DEBUG=OFF`.
3. Cross-platform validation: same replays valid across platforms.
