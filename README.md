<p align="center">
  <img src="assets/general-refresh_icon.png" alt="General Refresh Logo" width="128" height="128">
</p>

<h1 align="center">General Refresh</h1>

<p align="center">
  <strong>A modernized <em>Command &amp; Conquer: Generals / Zero Hour</em> built on <a href="https://github.com/fbraz3/GeneralsX">GeneralsX</a> — a native Vulkan renderer, Forward+ clustered lighting, a physical HDR post pipeline, AI-upscaled artwork, and truly smooth high-frame-rate gameplay.</strong>
</p>

<p align="center">
  <a href="https://github.com/Storm996/General-Refresh/releases"><img src="https://img.shields.io/github/v/release/Storm996/General-Refresh?include_prereleases&sort=date&display_name=tag&style=flat&label=Release" alt="GitHub Release"></a>
  <a href="https://github.com/fbraz3/GeneralsX"><img src="https://img.shields.io/badge/Built%20on-GeneralsX-blueviolet" alt="Built on GeneralsX"></a>
  <img src="https://img.shields.io/badge/Renderer-Vulkan%20(native)-red" alt="Native Vulkan Renderer">
  <img src="https://img.shields.io/badge/Lighting-Forward%2B%20%C2%B7%20512%20lights-orange" alt="Forward+ Lighting">
  <img src="https://img.shields.io/badge/Post-HDR%20%C2%B7%20ACES%20%C2%B7%20Bloom-cyan" alt="HDR Post Pipeline">
  <img src="https://img.shields.io/badge/Arch-64--bit-success" alt="64-bit">
  <img src="https://img.shields.io/badge/Frame%20Rate-Uncapped%20(render%20%2B%20unit%20motion)-brightgreen" alt="High Frame Rate">
  <img src="https://img.shields.io/badge/Platform-Windows-lightgrey" alt="Windows">
  <img src="https://img.shields.io/badge/License-GPLv3-yellow" alt="GPLv3">
</p>

<p align="center">
  <strong>English</strong> · <a href="README.zh-CN.md">简体中文</a>
</p>

---

## 🧭 What is this?

**General Refresh** is a derivative of the excellent **[GeneralsX](https://github.com/fbraz3/GeneralsX)** project — a modern, open-source re-port of *Command & Conquer: Generals* and *Zero Hour*.

GeneralsX modernized the **platform**: windowing, a 64-bit toolchain, a modern audio/video stack and cross-platform online play — so the game runs on today's systems. **General Refresh goes one step further: it rebuilds the renderer and relights the game.** We keep the deterministic simulation untouched and push the actual player experience forward:

### 🎨 Visual quality

The battlefield is drawn by a **native Vulkan renderer** — Forward+ clustered lighting, unified per-pixel light rig, sun and dynamic light shadow maps, volumetric fog, and an HDR post chain with ACES tone mapping, bloom and film grading. Combined with AI-refined textures, dot3 normal maps and PBR roughness materials, the 2003 fixed-function look is gone.

### 🕹️ Gameplay & experience

High-frame-rate battles, an RA3-style selection panel, a reworked mission UI, quick-launch of any mission, and 8 personality-style AI drivers.

### 🛠️ Platform & build

A **self-contained Vulkan backend** (no Direct3D runtime is loaded at all), a native 64-bit build, a modern audio/video stack (MiniAudio / OpenAL + FFmpeg), and an external WPF launcher that exposes the graphics settings.

> *Note: like GeneralsX, this is an engine re-port and requires original game data from a legitimate copy of Command & Conquer: Generals / Zero Hour.*

---

## ✅ Shipped Features

The following are **already implemented and playable**:

### 🖼️ Renderer & architecture

- 🎇 **Native Vulkan renderer** — the W3D drawing core runs on a self-built Vulkan backend (render targets, command buffers, descriptor sets, pipeline cache). No Direct3D runtime is loaded: the legacy device API is served by an internal stub, and all shaders are pre-baked GLSL → SPIR-V. There is no fixed-function ceiling left to hit — the whole pipeline is modern and programmable.
- 🧩 **Frame graph** — `W3DRenderGraph` owns the GPU lifetime of scene colour/depth, the overlay target, the five-level bloom chain and the water reflection image, and records real `vkCmdPipelineBarrier` transitions. Targets are rebuilt automatically after device loss.
- 💡 **Forward+ clustered lighting** — lighting data lives in SSBOs (the old uniform block exceeded the 64 KB limit on NVIDIA), with **512 light slots** (`fpLights[512]` + `fpLightExtra[512]`). Clustered and direct (256) paths both run; the per-frame budget defaults to 128 and lights are admitted by strength. Stress-tested at 300 lights / 1080p ≈ 157 fps (clustered) and 120 fps (direct).
- 🎨 **One unified light rig** — a single `W3DRenderLightingState` (sun, ambient, fog, height falloff, cloud shadow, exposure, revision) feeds terrain, roads, trees, vegetation, bibs, bridges, water, objects and the fixed-function path. Time-of-day and weather therefore affect every surface consistently, and atmosphere changes rebake the affected batches.
- ☀️ **GGX sun, hemisphere ambient, depth AO, direct-only shadows** — per-pixel GGX specular on terrain, roads, objects and point lights; hemisphere ambient (sky + ground) instead of a flat ambient term; depth-only AO (default 0.50); shadow maps attenuate sun light only, so ambient and emissive still hold the shadowed side.
- 🌗 **Shadow maps** — a 4096² sun shadow map with a **fixed 8192-unit world box, texel-snapped** (removes the shimmer while panning and the "shadows vanish when zooming" bug), terrain and roads as shadow casters, up to **4 dynamic light shadow maps** in the same frame, soft filtering and cloud shadows.
- 💡 **Point & spot lights** — inverse-square falloff with a corrected reference distance, real spot cones with inner/outer angles, an 8-cell light-cookie atlas plus procedural window gobos, vehicle headlight cones, and muzzle-flash / impact light pulses.
- 🌫️ **Volumetric fog (4 tiers)** — tier 0 is the retail distance haze, tier 1 an analytic exponential height integral along the eye ray, tiers 2/3 add 8/16 sun-shadow visibility samples (god-shaft style). Density, height, start distance and sun strength are exposed in the launcher.
- 🎞️ **HDR post pipeline** — the scene renders to `R16G16B16A16_SFLOAT`, followed by ACES/Reinhard tone mapping with exposure EV, a five-level bloom chain, CAS sharpening, auto exposure, a generated film LUT, SSAA 100/125/150/200 %, FXAA (3D only) and optional film grading (vignette, grain, saturation, contrast). The UI is always composited **after** the resolve, so post effects never blur or tint the HUD.
- 🖌️ **Materials** — tangent-space dot3 normal mapping, metallic-roughness PBR (rough-Blinn and GGX variants), and an AI texture-decomposition pipeline (CHORD) that produces `_nm` / `_rp` companion maps; texture atlas seams were fixed by making the terrain normal atlas single-level.
- 🌿 **World surfaces** — per-pixel roads, trees, vegetation, bibs and bridges (no more baked vertex lighting that ignored the sun), unified water material instance across sea/river/trapezoid with sky reflection and Forward+ point lights, an independent 5-face cubemap sky renderer, and reworked grass/foliage placement.
- 🎨 **AI-upscaled artwork** — unit and terrain textures re-painted and upscaled through an AI-assisted pipeline, replacing the blurry retail maps.

### 🕹️ Gameplay & experience

- 🏎️ **High-frame-rate gameplay** — the battle itself now runs smooth: unit locomotion, steering, turret aiming, weapon fire and animation are interpolated at the display refresh rate in Skirmish, Campaign and cinematic views — all as a presentation-layer improvement on top of the unchanged deterministic simulation, so online play, LAN and replays stay bit-for-bit faithful.
- 🖱️ **RA3-style unit selection panel** — when multiple units are selected, the bottom-left panel groups them by unit type with a live per-type count (e.g. *Overlord Tank x2*, *Battlemaster Tank x2*). Clicking one type icon selects every unit of that type at once, which makes commanding large mixed armies dramatically easier.
- 🧩 **Mission UI rework** — the mission briefing / selection interface has been redesigned.
- 🎬 **Quick-launch any mission** — pick **any mission directly from the main menu** and jump straight into it, without walking through the full campaign progression.
- 🤖 **AI personality-style drivers** — Skirmish AI now ships with 8 distinct personality styles on top of the difficulty layer (Offensive, Defensive, Support, Rush, Blitz, Horde, Fortress, Sneak) plus a neural-AI scaffold, selectable per AI slot. Brutal difficulty is also strengthened (halved team tempo, earlier rally windows), and a `GENERALX_AI_TRACE=1` trace channel records driver dispatch, team activation and fortification events for verification.

### 🛠️ Platform & build

- 🖥️ **Native 64-bit build (Windows x64)** — the Windows client runs on a 64-bit toolchain, breaking the legacy 32-bit process's 2GB/4GB address-space ceiling and **truly using all available system RAM and physical VRAM**.
- 🔊 **Audio & video backend modernization** — audio runs on **MiniAudio / OpenAL (x64)** instead of the 32-bit Miles engine, and intro/cutscene video is decoded by **FFmpeg**.
- 🚀 **External launcher** — a WPF launcher separates the pre-game UI from the engine and exposes the graphics stack: resolution and LOD, light budget, volumetric fog (tier, density, height, start, sun), HDR, tone mapping, bloom, AO, post AA, MSAA, GPU particles and weather, film grade and the rest of the advanced page.
- 🧪 **QA tooling** — five calibrated fixed camera positions, golden screenshots, PNG diff tooling, shader bake/light-layout checks and replay-CRC verification, so every visual change can be proven against the retail baseline.

### 🧪 Implemented but opt-in

These run on the shipped backend and are **off by default**; enable them in the launcher / Options.ini:

- 💥 **GPU combat particles** — compute + SSBO simulation feeding `vkCmdDrawIndirect`, spawned from retail additive batches (texture-preserving).
- 🌧️ **GPU weather** — rain, snow and sandstorm particle volumes driven camera-relative.
- 🌫️ **Soft particles** and 🔥 **heat haze** — scene-depth fade and screen-space distortion driven by live fire sources.
- 🔲 **MSAA 4x** — colour + depth resolve with alpha-to-coverage to follow; the UI is excluded from multisampling.
- 🌟 **God rays** — implemented and verified, but the sun is simply off-screen at the RTS camera angle, so it stays off unless a low-angle view asks for it.

---

## 🔭 In Progress & Roadmap

What we are actively building and planning next:

1. 🌊 **Screen-space reflections (SSR)** — needs a G-buffer normal/height pass first; the current post chain reconstructs normals from depth and is too noisy on grazing surfaces.
2. 🌤️ **Prefiltered IBL** — GGX-prefiltered sky cubemap with per-mip convolution, plus a roughness channel in the reflection interface.
3. ☁️ **Physical sky + aerial perspective** — a procedural atmosphere to replace the stitched cubemap.
4. 🔲 **TAA / SMAA** — both need extra buffers or LUT assets; the leaf/edge shimmer is expected to be better solved by MSAA + alpha-to-coverage first.
5. 🧱 **HBAO / GTAO** and **lit decals** — both blocked on the same G-buffer work as SSR.
6. 🌿 **World ecosystem systems** — richer foliage and a longer-term particle-effect upgrade (smoke, dust, explosions, weather).
7. 🗺️ **Mission map support** — expanded handling for mission/custom maps.

---

## 📰 Changelog

Every update is tracked by date in **[CHANGELOG.md](CHANGELOG.md)** — from the initial import and the 64-bit build to the Vulkan renderer rewrite, the lighting refactor and the 2.0 visual pass.

---

## 🚀 Getting Started

### Download & Play

Ready-to-run builds are published on the releases page:

* 📦 **[Download General Refresh Releases](https://github.com/Storm996/General-Refresh/releases)**
* 📖 Installation and game-data setup guides are available in the upstream Wiki lineage: [GeneralsX Wiki](https://github.com/fbraz3/GeneralsX/wiki)

### 🛠️ Building from Source

General Refresh builds with CMake presets. On Windows there is exactly one supported entry point — the **Vulkan core** build:

```bat
build-win64-vk.bat
:: Output: build\win64vk\GeneralsMD\Release\GeneralRefresh.exe

deploy-win64-vk.bat
:: Copies the exe next to your game data (previous exe is backed up first)
```

The base game (Generals) uses the same script with the `-Game Generals` switch, which builds `build\win64vk-generals` → `GeneralsX.exe`. Both the preset and the launcher hard-fail if the CMake cache is not Vulkan-core.

For the cross-platform Linux / macOS pipelines, see the [GeneralsX build guides](https://github.com/fbraz3/GeneralsX/wiki).

---

## 📜 Project Lineage & Credits

General Refresh stands on the shoulders of the RTS community and open-source pioneers:

* **[Westwood Studios & EA](https://www.ea.com/)** — for creating the Command & Conquer: Generals universe.
* **[TheSuperHackers](https://github.com/TheSuperHackers/GeneralsGameCode)** — foundational code modernization, stability fixes and upstream preservation.
* **[GeneralsX / fbraz3](https://github.com/fbraz3/GeneralsX)** — the cross-platform re-port that this project is directly forked from.
* **[Fighter19](https://github.com/Fighter19) & [feliwir](https://github.com/feliwir)** — early reference work pioneering modern platform and graphics in Zero Hour.

---

## 📄 License

See the [LICENSE](./LICENSE.md) file for details (GPLv3).
*EA has not endorsed and does not support this product. Command & Conquer is a trademark of Electronic Arts Inc.*
