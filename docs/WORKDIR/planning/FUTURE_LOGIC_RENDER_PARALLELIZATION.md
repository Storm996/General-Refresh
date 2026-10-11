# FUTURE: Logic/Render Parallelization & Performance Optimization Backlog

**Status**: ON HOLD (suspended by owner on 2026-10-05, Phase 0 instrumentation reverted)
**Target platform**: Windows 32-bit `win32-vcpkg` build running on Ryzen 7840HS + Radeon 780M iGPU, 4K display (144 Hz)
**Owner**: pending

---

## 1. Problem Statement

On the user's machine (7840HS + 780M iGPU with 2 GB allocated VRAM, ~1.1 GB actually used, 4K resolution, 144 Hz monitor, FPS option set to **No Limit**), the game:

- Runs at ~100 FPS, below the 144 Hz display limit.
- Shows ~80% GPU utilization — the remaining 20% is idle time waiting on the CPU/bandwidth, meaning the GPU is not the primary bottleneck.

The user's goal is to close the gap to 144 FPS. The chosen high-value direction is **Phase 6: logic/render parallelization** (high risk, high reward). This document records the analysis done so far, the Phase 0 measurement harness that was built and then reverted, and the pitfalls hit along the way.

## 2. Analysis Results (completed, no code changes)

### 2.1 Thread model: single-threaded serial main loop

`GameEngine::execute()` (`GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp`) runs a `while (!m_quitting)` loop on ONE thread:

```
input -> UI -> render (GameClient::UPDATE, includes TheDisplay->DRAW) -> messages/network
-> 30 Hz logic (GameLogic::UPDATE, every 3rd-4th render frame at 100+ fps)
-> FramePacer::update() (sleep/spin to hold FPS cap)
```

Key facts:

- Rendering, UI, input, radar, audio update and the whole simulation (AI, physics, particles, scripts) all run serially on the main thread.
- The only worker threads in the entire codebase are: NGMP network (GameNetworkingSockets), audio driver callbacks, screenshot capture, and update checker. There are **no** worker threads for rendering, logic, or AI — the 8 CPU cores are idle except one.
- `FramePacer::update()` ends each iteration with a sleep+spin wait (`FrameRateLimit::wait()`); with the cap disabled this wait is ~0, so the loop is purely render+logic bound.

### 2.2 Bottleneck assessment (hypothesis ranking)

1. **DX8 immediate-mode draw submission is single-core bound** — the D3D8 API layer serializes all draw calls through one thread; at 4K with the engine's multi-pass architecture this is the dominant cost.
2. **30 Hz logic spike** — `GameLogic::UPDATE()` (LOGICFRAMES_PER_SECOND = 30) runs a full simulation tick every 3-4 render frames at 100 fps; each tick (AI/pathfind/particles/collision) periodically stalls the frame and drags the average down.
3. **iGPU shared-memory bandwidth at 4K** — UMA bandwidth is shared between CPU and GPU; the engine's many fullscreen passes (scene + extra passes + the Bloom prototype's 3 additional passes) multiply bandwidth cost.

### 2.3 FPS cap investigation (mostly resolved)

- Code default is cap=60 (`GlobalData` defaults `m_useFpsLimit=TRUE`, `m_framesPerSecondLimit=60`; `GameMain.cpp` enables the limit), but the user runs at ~100 fps with the option set to **No Limit** and confirmed they do not want this re-verified. The campaign/`timeFast` path can bypass the limit; treat as "effectively uncapped".
- Conclusion: do not spend more time on the cap path. The 100 fps is a true performance ceiling, not a cap artifact.

## 3. Optimization Directions (risk-ordered)

| # | Direction | Risk | Notes |
|---|---|---|---|
| 1 | Turn off / downsample the Bloom prototype | Low | Quantifies post-process cost; skip if unused |
| 2 | Confirm cap/vsync state | Low | Done — user confirmed "No Limit" |
| 3 | Lower resolution test (e.g. 1440p) | Low | Confirms bandwidth hypothesis |
| 4 | Spread the 30 Hz logic spike across frames | Medium | Touches simulation pacing, CRC-sensitive |
| 5 | Draw-call batching / reduce passes | Medium | Engine-renderer work |
| 6 | **Logic/render parallelization** | **High** | Chosen direction — see below |

### Phase 6 sketch (logic/render parallel)

Target architecture:

```
[main thread = render thread]  input -> UI -> render (reads render snapshot) -> loop
                                         ^ snapshot pushed at end of each logic frame
[worker thread = logic thread] consume input commands -> GameLogic::UPDATE (30 Hz)
                               -> AI/physics/particles/scripts -> push snapshot -> wait
```

- Only `TheGameLogic->UPDATE()` and its subtree move to the worker; main thread keeps rendering, UI, input, audio, radar.
- The DX8 device is **not thread-safe**, so all rendering must stay on the main thread (this constraint is what makes the design hard).
- Biggest risks: **CRC determinism and replay compatibility** (the logic thread must consume commands in exactly the same order as today), plus the massive amount of global state (`The*` singletons) touched by both sides.
- This is a multi-week effort; do not attempt without the Phase 0 measurement baseline first.

## 4. Phase 0: HUD Frame-Time Instrumentation (implemented, then reverted on request)

Goal: show a live frame-time breakdown in the HUD so the render/logic/wait split can be observed while playing.

### 4.1 Design

- **FrameRateLimit** (`Core/GameEngine/Include|Source/Common/FrameRateLimit.*`): report how long the last `wait()` actually blocked (sleep + spin) via `getLastWaitSeconds()`.
- **FramePacer** (`Core/GameEngine/Include|Source/Common/FramePacer.*`): three exponentially smoothed averages in ms — render, logic, wait (`addRenderTimeMs/addLogicTimeMs/addWaitTimeMs`, `getAverage*Ms`).
- **GameEngine::update()**: time the `TheGameClient->UPDATE()` segment (render) and the `TheGameLogic->UPDATE()` segment (logic), feed both into FramePacer. Wall-clock timing is display-only and never touches the simulation (no CRC/replay impact).
- **InGameUI**: a second HUD line under the existing FPS counter showing `R <render ms>  L <logic ms>  W <wait ms>`, reusing the existing `DisplayString` pattern (`m_frameTimeStatsString`), created in `refreshRenderFpsResources()`, updated in `updateRenderFpsString()`, drawn in `drawRenderFps()` (both anchored and non-anchored branches).

### 4.2 Files touched (all reverted as of 2026-10-05)

| File | Change |
|---|---|
| `Core/GameEngine/Include/Common/FrameRateLimit.h` | `getLastWaitSeconds()`, `m_lastWaitSeconds` |
| `Core/GameEngine/Source/Common/FrameRateLimit.cpp` | measure blocking time in `wait()` |
| `Core/GameEngine/Include/Common/FramePacer.h` | 3 add methods, 3 getters, 3 `m_avg*` members |
| `Core/GameEngine/Source/Common/FramePacer.cpp` | smoothed averages, feed wait time in `update()` |
| `GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp` | `std::chrono` timing around client update and logic update |
| `GeneralsMD/Code/GameEngine/Include/GameClient/InGameUI.h` | `m_frameTimeStatsString`, `m_lastFrameTimeStatsValid` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp` | init/free/refresh/update/draw of the stats line |

### 4.3 Pitfalls encountered (all confirmed by compiler/runtime evidence)

1. **`GetPrecisionTimer` / `GetPrecisionTimerTicksPerSec` are compiled out in Release builds** — they are declared in `Common/PerfTimer.h` only under `#if defined(PERF_TIMERS) || defined(DUMP_PERF_STATS)`. Using them unguarded in `GameEngine::update()` produced C3861 errors. Fix: use `std::chrono::steady_clock` (already used elsewhere, cross-platform, safe for display-only metrics).
2. **Header/implementation drift**: the first patch added `addWaitTimeMs` only to the `.cpp`, missing the declaration in `FramePacer.h` — C3861/C2039 errors at the call site and in the member function definition. When adding a new method, update both files in the same edit.
3. **Editing `FramePacer.h` triggers a large rebuild** — it is included very widely (~175 targets recompile in the x86 Hostx86 toolchain, several minutes). Plan header edits to land once, fully declared.
4. **Build environment specifics** (Windows sandbox): `git` is not on PATH (use the VS-bundled git under `Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd\git.exe`); builds must run inside `Enter-VsDevShell -DevCmdArguments '-arch=x86'` (Ninja Multi-Config + MSVC x86); PowerShell does not support `&&` (use `;`).
5. **The workspace has no `.git`** (source was copied directly), so there is no way to revert via git — reverts must be done by hand with precise reverse edits. Keep a mental diff of every change made.

## 5. Revert Note (2026-10-05)

- All Phase 0 instrumentation was reverted by hand (7 files listed above) at the owner's request; earlier work (Bloom prototype + its two bugfixes in `W3DShaderManager.*` / `W3DView.cpp` / `CommandXlat.h`, and the FPS option / campaign lock-frame work) was **not** touched.
- The deployed exe (`GeneralsXZH.exe` in the game dir and `dist/`) is the Bloom-fixed build; after the revert the source tree matches that binary, so no rebuild is required.

## 6. Restart Checklist

1. Re-apply the instrumentation from section 4 (or diff against section 4.2).
2. Build: `Enter-VsDevShell -DevCmdArguments '-arch=x86'` then `cmake --build i:\Projects\GeneralX\build\win32-vcpkg --target z_generals --config Release --parallel 6`.
3. Deploy: copy `build/win32-vcpkg/GeneralsMD/GeneralsXZH.exe` to the game directory and `dist/`.
4. Verify in-game: second HUD line under the FPS counter shows `R / L / W` ms; watch the R vs L split during combat and camera movement at 4K.
5. Use the measured R/L/W numbers to decide the actual Phase 6 approach before writing any parallelization code.
