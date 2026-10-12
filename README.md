<p align="center">
  <img src="assets/general-refresh_icon.png" alt="General Refresh Logo" width="128" height="128">
</p>

<h1 align="center">General Refresh</h1>

<p align="center">
  <strong>A high-frame-rate enhanced fork of <a href="https://github.com/fbraz3/GeneralsX">GeneralsX</a> — bringing <em>truly</em> smooth 60/120/144+ FPS gameplay (including unit motion, steering and firing) to Command&nbsp;&&nbsp;Conquer: Generals &amp; Zero Hour on modern Windows.</strong>
</p>

<p align="center">
  <a href="https://github.com/Storm996/General-Refresh/releases"><img src="https://img.shields.io/github/v/release/Storm996/General-Refresh?include_prereleases&sort=date&display_name=tag&style=flat&label=Release" alt="GitHub Release"></a>
  <a href="https://github.com/fbraz3/GeneralsX"><img src="https://img.shields.io/badge/Built%20on-GeneralsX-blueviolet" alt="Built on GeneralsX"></a>
  <img src="https://img.shields.io/badge/Frame%20Rate-Uncapped%20(render%20%2B%20unit%20motion)-brightgreen" alt="High Frame Rate">
  <img src="https://img.shields.io/badge/Platform-Windows-lightgrey" alt="Windows">
  <img src="https://img.shields.io/badge/License-GPLv3-yellow" alt="GPLv3">
</p>

<p align="center">
  <strong>English</strong> · <a href="README.zh-CN.md">简体中文</a>
</p>

---

## 🆕 What's New (Oct 2026)

**The renderer has been ported from Direct3D 8 to Direct3D 9.** The `DX8Wrapper` core now drives the modern `IDirect3D9` / `IDirect3DDevice9` interfaces, loaded at runtime straight from `d3d9.dll`. This lifts the programmable-shader ceiling from `ps_1_4` toward `ps_3_0` and unlocks multi-light forward shading, HDR and bloom-class effects for upcoming passes — while preserving the exact fixed-pipeline / texture-stage behaviour, so gameplay, LAN and replays stay bit-for-bit faithful. The native 64-bit Windows client now runs entirely on the D3D9 path.

---

## 🧭 What is this?

**General Refresh** is a derivative of the excellent **[GeneralsX](https://github.com/fbraz3/GeneralsX)** project — a modern, open-source re-port of *Command & Conquer: Generals* and *Zero Hour*. We build directly on top of that foundation and take it one big step further on the **frame-rate front**.

GeneralsX already modernized the platform stack (SDL3 + DXVK/Vulkan + OpenAL/MiniAudio, 64-bit, cross-platform online play via GeneralsOnline/NGMP). On top of that, **General Refresh focuses on delivering a genuinely high-frame-rate experience end-to-end** — not just the camera and menus, but the **battle itself**.

> *Note: like GeneralsX, this is an engine re-port and requires original game data from a legitimate copy of Command & Conquer: Generals / Zero Hour.*

---

## 🏎️ Our Flagship Contribution: True High-Frame-Rate Gameplay

This is the core reason General Refresh exists as its own project.

The original game runs its entire simulation — logic, unit movement, turning, and firing — on a **locked 30 FPS** clock. GeneralsX decoupled the **rendering** from that 30 Hz simulation, so the view and UI could run at high refresh rates. However, the **units themselves were still stepping along the 30 FPS simulation grid**: on a 120/144 Hz display, tank treads, turret aiming, marching infantry and muzzle flashes still visibly stuttered, because the *motion of units over time* was never promoted onto the render clock.

**General Refresh closes that gap.** We extended high-frame-rate smoothness down into the **per-unit motion pipeline**, so the following now interpolate smoothly at the display refresh rate:

- 🪖 **Unit locomotion** — walking/driving movement is continuous instead of 30 fps "stop-motion".
- 🎯 **Steering & turret rotation** — turning, aiming and barrel tracking are fluid.
- 🔫 **Weapon fire & animation** — muzzle flashes, firing poses and weapon cycles no longer snap between 30 Hz frames.
- 🎥 **Every camera context** — smooth in **Skirmish (vs AI)**, in **single-player Campaign missions**, and in **cinematic / free-camera** views.

Crucially, all of this is done as a **presentation-layer / interpolation improvement**. The underlying game simulation stays on its deterministic lockstep grid, so **online play, LAN and replay compatibility remain bit-for-bit faithful** (no SyncCrash regressions). Fast hardware = smoother eyes, not a changed battlefield clock.

| Aspect | Upstream GeneralsX | **General Refresh** |
|---|---|---|
| Rendering / UI at high FPS | ✅ | ✅ |
| Camera & menu smoothness | ✅ | ✅ |
| **Unit movement at high FPS** | ❌ (still 30 Hz) | ✅ |
| **Unit turning / turret aiming** | ❌ (still 30 Hz) | ✅ |
| **Firing & weapon animation** | ❌ (still 30 Hz) | ✅ |
| Skirmish / Campaign / Cinematic all covered | partial | ✅ |

---

## ✅ Shipped Features

Beyond high frame rate, the following are **already implemented and playable**:

- 🧱 **Normal-mapped (dot3) rendering** — a tangent-space dot3 normal-mapping path is wired into the W3D renderer, giving surfaces real per-pixel lighting depth. *Experimental and user-toggleable:* it is **OFF by default** (still being hardened) and can be enabled live from **Options → the "Normal Mapping" checkbox** without a restart.
- 🎨 **Texture repaints / upscaling** — unit and terrain artwork has been re-painted and upscaled through an AI-assisted texture pipeline, replacing blurry low-res retail textures with crisper, higher-detail maps.
- 🗺️ **Mission map support** — expanded handling for mission/custom maps.
- 🧩 **Mission UI rework** — the mission briefing / selection interface has been redesigned.
- 🎬 **Quick-launch any mission** — pick **any mission directly from the main menu** and jump straight into it, without walking through the full campaign progression.
- 🖥️ **Native 64-bit build (Windows x64)** — the Windows client has been fully ported to a 64-bit toolchain, breaking the legacy 32-bit process's 2GB/4GB address-space ceiling and **truly using all available system RAM and physical VRAM** — removing the capacity bottleneck for larger maps, HD assets and heavy particle loads. (Since Microsoft never shipped a 64-bit Direct3D8 runtime, we achieve this via DXVK's x64 `d3d8.dll` shim that translates D3D8 to Vulkan at runtime.)
- 🎇 **Direct3D9 renderer** — the `DX8Wrapper` core has been ported from Direct3D8 to the modern Direct3D9 (`IDirect3D9`/`IDirect3DDevice9`, loaded at runtime from `d3d9.dll`), lifting the shader ceiling from `ps_1_4` toward `ps_3_0` and paving the way for multi-light, HDR and bloom effects without touching fixed-pipeline behaviour or replay compatibility.

---

## 🔭 In Progress & Roadmap

What we are actively building and planning next:

1. 🌿 **World ecosystem systems** — enriching the environment with living systems, currently a **grass/foliage system** (in progress) and a planned **particle-effect upgrade** (smoke, dust, explosions, weather).
2. 🤖 **AI-driven opponents** — adding **artificial-intelligence-powered battle AI** to bring a wider variety of AI personalities and play styles to Skirmish and beyond.
3. ✨ **Next-gen rendering effects** — with the Direct3D9 port now shipped, building the multi-light forward shading, HDR and high-quality bloom passes it unlocked, and pursuing a **native Vulkan / D3D11 backend** as the long-term goal.
4. 🔊 **Audio & video backend modernization** — completing the 64-bit platform layer by moving to **MiniAudio / OpenAL (x64)** audio and **FFmpeg** for intro/cutscene playback.

---

## 🚀 Getting Started

### Download & Play
Ready-to-run builds are published on the releases page:

* 📦 **[Download General Refresh Releases](https://github.com/Storm996/General-Refresh/releases)**
* 📖 Installation and game-data setup guides are available in the upstream Wiki lineage: [GeneralsX Wiki](https://github.com/fbraz3/GeneralsX/wiki)

### 🛠️ Building from Source
General Refresh uses CMake presets (inherited from GeneralsX). Windows (MSVC) example:

```bash
cmake --preset win32
cmake --build build/win32-vcpkg --config Release --target z_generals
# Output: build/win32-vcpkg/GeneralsMD/Release/GeneralRefresh.exe
```

For full prerequisites and the cross-platform Linux / macOS pipelines that GeneralsX provides, see the [GeneralsX build guides](https://github.com/fbraz3/GeneralsX/wiki).

---

## 📜 Project Lineage & Credits

General Refresh stands on the shoulders of the RTS community and open-source pioneers:

* **[Westwood Studios & EA](https://www.ea.com/)** — for creating the Command & Conquer: Generals universe.
* **[TheSuperHackers](https://github.com/TheSuperHackers/GeneralsGameCode)** — foundational code modernization, stability fixes and upstream preservation.
* **[GeneralsX / fbraz3](https://github.com/fbraz3/GeneralsX)** — the cross-platform SDL3 + DXVK + OpenAL re-port that this project is directly forked from.
* **[Fighter19](https://github.com/Fighter19) & [feliwir](https://github.com/feliwir)** — early reference work pioneering SDL3, DXVK and OpenAL in Zero Hour.

---

## 💖 Contributing

Contributions, bug reports and ideas are welcome — please open an issue or pull request after reviewing the guidelines in [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`AGENTS.md`](AGENTS.md).

## 📄 License

See the [LICENSE](./LICENSE.md) file for details (GPLv3).
*EA has not endorsed and does not support this product. Command & Conquer is a trademark of Electronic Arts Inc.*
