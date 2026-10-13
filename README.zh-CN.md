<p align="center">
  <img src="assets/general-refresh_icon.png" alt="General Refresh 图标" width="128" height="128">
</p>

<h1 align="center">General Refresh</h1>

<p align="center">
  <strong>基于 <a href="https://github.com/fbraz3/GeneralsX">GeneralsX</a> 的《命令与征服：将军 / 绝命时刻》现代化增强版 —— 原生 64 位、Direct3D9 渲染器与 PS 3.0 着色器天花板、AI 高清贴图、现代指挥系统，以及真正流畅的高帧率游戏体验。</strong>
</p>

<p align="center">
  <a href="https://github.com/Storm996/General-Refresh/releases"><img src="https://img.shields.io/github/v/release/Storm996/General-Refresh?include_prereleases&sort=date&display_name=tag&style=flat&label=Release" alt="GitHub 版本"></a>
  <a href="https://github.com/fbraz3/GeneralsX"><img src="https://img.shields.io/badge/基于-GeneralsX-blueviolet" alt="基于 GeneralsX"></a>
  <img src="https://img.shields.io/badge/渲染器-D3D9%20%2F%20PS%203.0-blue" alt="D3D9 渲染器">
  <img src="https://img.shields.io/badge/架构-64%20位-success" alt="64 位">
  <img src="https://img.shields.io/badge/帧率-不设上限%20(渲染%20%2B%20单位运动)-brightgreen" alt="高帧率">
  <img src="https://img.shields.io/badge/平台-Windows-lightgrey" alt="Windows">
  <img src="https://img.shields.io/badge/许可-GPLv3-yellow" alt="GPLv3">
</p>

<p align="center">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

---

## 🧭 这是什么？

**General Refresh** 是优秀开源项目 **[GeneralsX](https://github.com/fbraz3/GeneralsX)** 的衍生版——一个现代化的《命令与征服：将军》与《绝命时刻》引擎重制项目。

GeneralsX 完成了**平台层**的现代化：SDL3 窗口、DXVK/Vulkan 渲染、OpenAL/MiniAudio 音频、64 位工具链与跨平台联机——让游戏能在今天的系统上运行。**General Refresh 则更进一步：让游戏本身现代化。** 我们站在这个基础之上，把玩家体验真正往前推进：

- 🎇 **Direct3D9 渲染器 + PS 3.0-现有方案的着色器天花板** —— 替换遗留的 D3D8 / `ps_1_4` 管线，解锁现代光影效果。
- 🏎️ **真正流畅的高帧率战场** —— 上游 GeneralsX 把渲染从 30 Hz 模拟中解耦了出来，但单位仍踩着 30 FPS 的网格步进。General Refresh 把单位位移、转向、炮塔瞄准与开火都按显示刷新率插值，而确定性模拟（进而联机、局域网与回放）保持逐位忠实。
- 🖱️ **现代指挥系统** —— 新增各类操作便利功能，例如《红警 3》式选中单位构成面板。
- 🧱 **法线贴图（dot3）表面**与 🎨 **AI 辅助贴图重绘 / 超分**，画面大幅更清晰。
- 🖥️ **原生 64 位 Windows 构建** —— 突破遗留 2GB/4GB 地址空间墙，用满可用的内存与显存。

> *说明：与 GeneralsX 一样，本项目是引擎重制，需要《命令与征服：将军 / 绝命时刻》正版副本的原始游戏数据。*

---

## ✅ 已交付的特性

以下内容**均已实现并可玩**：

- 🖱️ **《红警 3》式选中单位构成面板** —— 框选多个单位后，左下角面板会按单位类别分组显示图标与实时数量（例如"炎黄坦克 x2"、"战神坦克 x2"）；点击某个类别图标即可一键全选该类别全部单位，让大规模混编部队的指挥变得格外轻松。
- 🎇 **Direct3D9 渲染器** —— `DX8Wrapper` 核心已从 Direct3D8 端口到现代的 Direct3D9（`IDirect3D9`/`IDirect3DDevice9`，运行期从 `d3d9.dll` 加载），把着色器天花板从 `ps_1_4` 抬向 `ps_3_0`，为多灯、HDR 与 Bloom 效果铺路，同时不改动固定管线行为与回放兼容性。
- 🖥️ **原生 64 位（Windows x64）构建** —— Windows 客户端已全面移植到 64 位工具链，彻底突破遗留 32 位进程的 2GB/4GB 地址空间墙，**真正用满可用的系统内存与物理显存**。（由于微软从未提供 64 位的 Direct3D8 运行时，我们借助 DXVK 的 x64 `d3d8.dll` 垫片在运行期将 D3D8 实时翻译为 Vulkan 来达成这一点。）
- 🏎️ **高帧率游戏体验** —— 战场本身如今跑得顺畅：单位位移、转向、炮塔瞄准、开火与动画都在遭遇战、战役与电影镜头下按显示刷新率插值——全部是建立在不变确定性模拟之上的表现层改进，因此联机、局域网与回放保持逐位忠实。
- 🧱 **法线贴图（dot3）渲染** —— 切线空间 dot3 法线渲染路径已接入 W3D 渲染器，让表面拥有真正的逐像素光照立体感。*实验性、可开关：* 目前**默认关闭**（仍在持续打磨），可在 **选项 → "法线贴图"复选框**即时开启，无需重启。
- 🎨 **贴图重绘 / 超分** —— 单位与地形的贴图通过一条 AI 辅助的贴图管线被重绘并放大，替换掉模糊的低分辨率原版贴图，带来更清晰、更高细节的画面。
- 🗺️ **任务地图支持** —— 扩展了对任务 / 自定义地图的处理。
- 🧩 **任务界面改版** —— 任务简报 / 选择界面经过重新设计。
- 🎬 **直接进入任意任务** —— 可在**主菜单直接选择任意任务**并立即进入，无需沿整条战役进度一路推进。
- 🔊 **音视频后端现代化** —— 随 64 位切换一同完成：音频从 32 位 Miles 全面转向 **MiniAudio / OpenAL（x64）**，开场视频与过场动画由 **FFmpeg** 解码播放，64 位平台层收尾完成。

---

## 🔭 进行中与路线图

我们正在构建以及下一步计划的内容：

1. ✨ **光影升级** —— 法线贴图、阴影柔化、Bloom 与逐像素多光源均已实现于开发分支，即将随下一次更新正式交付；**原生 Vulkan / D3D11 后端**仍为长期目标。
2. 🌿 **世界生态系统** —— 用"有生命的"系统丰富场景环境，目前正在进行**草丛 / 植被系统**，并计划升级**粒子特效**（烟雾、尘土、爆炸、天气）。
3. 🤖 **AI 驱动的对手** —— 引入**由人工智能驱动的战斗 AI**，为遭遇战及更多场景带来更丰富的 AI 个性与打法风格。

---

## 📰 更新日志

每一次更新都按日期记录在 **[CHANGELOG.md](CHANGELOG.md)** 中——从初始导入到 64 位构建、D3D9、PS 3.0 以及更远，哪一天交付了什么一目了然。

---

## 🚀 快速上手

### 下载与游玩
可直接运行的构建已发布于版本页面：

* 📦 **[下载 General Refresh 版本](https://github.com/Storm996/General-Refresh/releases)**
* 📖 安装与游戏数据配置指南可参阅上游 Wiki：[GeneralsX Wiki](https://github.com/fbraz3/GeneralsX/wiki)

### 🛠️ 从源码构建
General Refresh 沿用 GeneralsX 的 CMake presets。Windows（MSVC）示例：

```bash
cmake --preset win32
cmake --build build/win32-vcpkg --config Release --target z_generals
# 产物：build/win32-vcpkg/GeneralsMD/Release/GeneralRefresh.exe
```

完整前置依赖，以及 GeneralsX 提供的 Linux / macOS 跨平台流水线，请参阅 [GeneralsX 构建指南](https://github.com/fbraz3/GeneralsX/wiki)。

---

## 📜 项目血脉与致谢

General Refresh 站立在 RTS 社区与开源先驱的肩膀之上：

* **[Westwood Studios 与 EA](https://www.ea.com/)** —— 创作了《命令与征服：将军》的世界。
* **[TheSuperHackers](https://github.com/TheSuperHackers/GeneralsGameCode)** —— 基础的代码现代化、稳定性修复与上游保存。
* **[GeneralsX / fbraz3](https://github.com/fbraz3/GeneralsX)** —— 本项目直接 fork 自的跨平台 SDL3 + DXVK + OpenAL 重制引擎。
* **[Fighter19](https://github.com/Fighter19) 与 [feliwir](https://github.com/feliwir)** —— 率先在《绝命时刻》中引入 SDL3、DXVK 与 OpenAL 的参考性工作。

---


## 📄 许可

详见 [LICENSE](./LICENSE.md)（GPLv3）。
*EA 并未认可亦不支持本产品。Command & Conquer 是 Electronic Arts Inc. 的商标。*
