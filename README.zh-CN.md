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

### 🎨 画质优化

AI 精修贴图、法线贴图（dot3）、逐像素光照与 PBR 粗糙度材质，让战场画面大幅更清晰、更有立体感。

### 🕹️ 游戏特性与体验

高帧率战场、红警 3 式选中面板、任务界面改版、主菜单直进任意任务，以及 8 种风格的 AI 驱动。

### 🛠️ 平台与构建

Direct3D9 渲染器（PS 3.0 着色器天花板）、原生 64 位构建与现代音视频后端（MiniAudio / OpenAL + FFmpeg）。

> *说明：与 GeneralsX 一样，本项目是引擎重制，需要《命令与征服：将军 / 绝命时刻》正版副本的原始游戏数据。*

---

## ✅ 已交付的特性

以下内容**均已实现并可玩**：

### 🎨 画质优化

- 🎨 **贴图重绘 / 超分** —— 单位与地形的贴图通过一条 AI 辅助的贴图管线被重绘并放大，替换掉模糊的低分辨率原版贴图，带来更清晰、更高细节的画面。
- 🧱 **法线贴图（dot3）渲染** —— 切线空间 dot3 法线渲染路径已接入 W3D 渲染器，让表面拥有真正的逐像素光照立体感。可在 **选项 → “法线贴图”复选框**即时开关，无需重启。
- 💡 **逐像素光照** —— 战场刚性网格的光照从逐顶点升级为逐像素：按插值后的世界空间几何法线逐像素重算批次自身的多盏方向光（最多 4 盏）与 Blinn-Phong 高光，坦克与建筑的明暗交界更锐利；环境底光默认压至 0.5 增强明暗对比。默认开启，`GENERALSX_GEOMNORMAL=0` 回退逐顶点。
- 🔩 **基于物理的材质（PBR 粗糙度）渲染** —— 金属-粗糙度 PBR 路径已接入 dot3 光照管线：`GENERALSX_PBR=1` 选 rough-Blinn 变体、`=2` 选 GGX 变体；缺少 `_rp` 材质图的表面自动回退到常数粗糙/金属值。资产由一条 AI 贴图分解管线（CHORD）产出——把 diffuse 贴图分解为 basecolor / 法线 / 粗糙度 / 金属度四通道并打包为引擎 `_rp.tga` 图，实机验证 `[PBR] variant compiled/installed/mounted` 全链路成立，回放 CRC 对照证明与逻辑无关。

### 🕹️ 游戏特性与体验

- 🏎️ **高帧率游戏体验** —— 战场本身如今跑得顺畅：单位位移、转向、炮塔瞄准、开火与动画都在遭遇战、战役与电影镜头下按显示刷新率插值——全部是建立在不变确定性模拟之上的表现层改进，因此联机、局域网与回放保持逐位忠实。
- 🖱️ **《红警 3》式选中单位构成面板** —— 框选多个单位后，左下角面板会按单位类别分组显示图标与实时数量（例如“炎黄坦克 x2”、“战神坦克 x2”）；点击某个类别图标即可一键全选该类别全部单位，让大规模混编部队的指挥变得格外轻松。
- 🧩 **任务界面改版** —— 任务简报 / 选择界面经过重新设计。
- 🎬 **直接进入任意任务** —— 可在**主菜单直接选择任意任务**并立即进入，无需沿整条战役进度一路推进。
- 🤖 **AI 风格化驱动升级** —— 遭遇战 AI 在难度层之上新增 8 种可选个性风格（进攻、防守、支援、疯狗 Rush、闪击、人海、铁壁、老六潜入）外加神经 AI 脚手架，每个 AI 槽位可独立选择；残暴难度同步强化（组队节奏减半、集结窗口提前），`GENERALX_AI_TRACE=1` 取证通道可记录驱动分派、队伍激活与防御工事事件。

### 🛠️ 平台与构建

- 🎇 **Direct3D9 渲染器** —— `DX8Wrapper` 核心已从 Direct3D8 端口到现代的 Direct3D9（`IDirect3D9`/`IDirect3DDevice9`，运行期从 `d3d9.dll` 加载），把着色器天花板从 `ps_1_4` 抬向 `ps_3_0`，为多灯、HDR 与 Bloom 效果铺路，同时不改动固定管线行为与回放兼容性。
- 🖥️ **原生 64 位（Windows x64）构建** —— Windows 客户端已全面移植到 64 位工具链，彻底突破遗留 32 位进程的 2GB/4GB 地址空间墙，**真正用满可用的系统内存与物理显存**——不再需要 32 位 D3D8 垫片：Direct3D9 渲染器直接运行在原生 64 位运行时之上。
- 🔊 **音视频后端现代化** —— 随 64 位切换一同完成：音频从 32 位 Miles 全面转向 **MiniAudio / OpenAL（x64）**，开场视频与过场动画由 **FFmpeg** 解码播放，64 位平台层收尾完成。

---

## 🔭 进行中与路线图

我们正在构建以及下一步计划的内容：

1. 🌿 **世界生态系统** —— 用"有生命的"系统丰富场景环境，目前正在进行**草丛 / 植被系统**，并计划升级**粒子特效**（烟雾、尘土、爆炸、天气）。
2. 🗺️ **任务地图支持** —— 扩展对任务 / 自定义地图的处理。
3. 🎮 **原生 Vulkan / D3D11 渲染后端** —— 现有 D3D9 管线之后的长期目标。

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
