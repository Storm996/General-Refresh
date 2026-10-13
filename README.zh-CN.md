<p align="center">
  <img src="assets/general-refresh_icon.png" alt="General Refresh 图标" width="128" height="128">
</p>

<h1 align="center">General Refresh</h1>

<p align="center">
  <strong>基于 <a href="https://github.com/fbraz3/GeneralsX">GeneralsX</a> 的高帧率增强衍生版 —— 为《命令与征服：将军》与《绝命时刻》在现代 Windows 上带来<em>真正</em>流畅的 60/120/144+ FPS 游戏体验（涵盖单位运动、转向与开火）。</strong>
</p>

<p align="center">
  <a href="https://github.com/Storm996/General-Refresh/releases"><img src="https://img.shields.io/github/v/release/Storm996/General-Refresh?include_prereleases&sort=date&display_name=tag&style=flat&label=Release" alt="GitHub 版本"></a>
  <a href="https://github.com/fbraz3/GeneralsX"><img src="https://img.shields.io/badge/基于-GeneralsX-blueviolet" alt="基于 GeneralsX"></a>
  <img src="https://img.shields.io/badge/帧率-不设上限%20(渲染%20%2B%20单位运动)-brightgreen" alt="高帧率">
  <img src="https://img.shields.io/badge/平台-Windows-lightgrey" alt="Windows">
  <img src="https://img.shields.io/badge/许可-GPLv3-yellow" alt="GPLv3">
</p>

<p align="center">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

---

## 🆕 本次更新（2026 年 10 月）

**渲染器已从 Direct3D 8 端口到 Direct3D 9。** `DX8Wrapper` 核心如今驱动现代的 `IDirect3D9` / `IDirect3DDevice9` 接口，直接于运行期从 `d3d9.dll` 加载。这把可编程着色器天花板从 `ps_1_4` 抬向 `ps_3_0`，为后续的渲染 pass 解锁了多灯前向着色、HDR 与 Bloom 级别的效果——同时完整保留了固定管线 / 纹理阶段的行为，因此玩法、局域网与回放保持逐位忠实。原生 64 位 Windows 客户端现已完全运行在 D3D9 路径上。

---

## 🧭 这是什么？

**General Refresh** 是优秀开源项目 **[GeneralsX](https://github.com/fbraz3/GeneralsX)** 的衍生版——一个现代化的《命令与征服：将军》与《绝命时刻》引擎重制项目。我们直接构建在其基础之上，并在**帧率**这条路线上把它推进了大大的一步。

GeneralsX 已经完成了平台栈的现代化改造（SDL3 + DXVK/Vulkan + OpenAL/MiniAudio、64 位、通过 GeneralsOnline/NGMP 实现的跨平台联机）。在此基础上，**General Refresh 专注于交付一套端到端、名副其实的高帧率体验**——不只是镜头和菜单，而是**整个战场本身**。

> *说明：与 GeneralsX 一样，本项目是引擎重制，需要《命令与征服：将军 / 绝命时刻》正版副本的原始游戏数据。*

---

## 🏎️ 核心贡献：真正的高帧率游戏体验

这正是 General Refresh 作为一个独立项目存在的根本原因。

原版游戏把整套模拟——逻辑、单位移动、转向、开火——全部锁死在 **30 FPS** 的时钟上。GeneralsX 把**渲染**从这套 30 Hz 模拟中解耦了出来，于是画面与 UI 可以跑到高刷新率。然而，**单位本身依旧沿着 30 FPS 的模拟网格在步进**：在 120/144 Hz 显示器上，坦克行进、炮塔瞄准、步兵行军、枪口火光仍然肉眼可见地一卡一卡——因为"单位随时间的运动"从未被提升到渲染时钟上。

**General Refresh 补上了这一环。** 我们把高帧率的平滑度一路下探到了**逐单位的运动管线**，使以下内容如今都能按显示器刷新率平滑插值：

- 🪖 **单位位移** —— 行走 / 行驶的运动是连续的，而不是 30 fps 的"定格动画"。
- 🎯 **转向与炮塔旋转** —— 转身、瞄准、炮管跟踪都如丝般顺滑。
- 🔫 **开火与武器动画** —— 枪口火光、开火姿态与武器循环不再在 30 Hz 帧之间硬跳。
- 🎥 **覆盖所有镜头场景** —— 在**遭遇战（对战 AI）**、**单人战役任务**以及**电影 / 自由镜头**下都同样流畅。

至关重要的是，以上全部是**表现层 / 插值层面**的改进。底层游戏模拟仍运行在其确定性锁步网格之上，因此**联机、局域网与回放的兼容性保持逐位（bit-for-bit）忠实**（不引入任何 SyncCrash 回归）。更快的硬件 = 更顺滑的视觉，而不是一台被改写的战场时钟。

| 维度 | 上游 GeneralsX | **General Refresh** |
|---|---|---|
| 渲染 / UI 高帧率 | ✅ | ✅ |
| 镜头与菜单流畅度 | ✅ | ✅ |
| **单位运动高帧率** | ❌（仍是 30 Hz） | ✅ |
| **单位转向 / 炮塔瞄准** | ❌（仍是 30 Hz） | ✅ |
| **开火与武器动画** | ❌（仍是 30 Hz） | ✅ |
| 遭遇战 / 战役 / 电影全覆盖 | 部分 | ✅ |

---

## ✅ 已交付的特性

除高帧率之外，以下内容**均已实现并可玩**：

- 🧱 **法线贴图（dot3）渲染** —— 切线空间 dot3 法线渲染路径已接入 W3D 渲染器，让表面拥有真正的逐像素光照立体感。*实验性、可开关：* 目前**默认关闭**（仍在持续打磨），可在 **选项 → "法线贴图"复选框**即时开启，无需重启。
- 🎨 **贴图重绘 / 超分** —— 单位与地形的贴图通过一条 AI 辅助的贴图管线被重绘并放大，替换掉模糊的低分辨率原版贴图，带来更清晰、更高细节的画面。
- 🗺️ **任务地图支持** —— 扩展了对任务 / 自定义地图的处理。
- 🧩 **任务界面改版** —— 任务简报 / 选择界面经过重新设计。
- 🎬 **直接进入任意任务** —— 可在**主菜单直接选择任意任务**并立即进入，无需沿整条战役进度一路推进。
- 🖥️ **原生 64 位（Windows x64）构建** —— Windows 客户端已全面移植到 64 位工具链，彻底突破遗留 32 位进程的 2GB/4GB 地址空间墙，**真正用满可用的系统内存与物理显存**——为更大地图、高清贴图与繁重粒子负载解除容量瓶颈。（由于微软从未提供 64 位的 Direct3D8 运行时，我们借助 DXVK 的 x64 `d3d8.dll` 垫片在运行期将 D3D8 实时翻译为 Vulkan 来达成这一点。）
- 🎇 **Direct3D9 渲染器** —— `DX8Wrapper` 核心已从 Direct3D8 端口到现代的 Direct3D9（`IDirect3D9`/`IDirect3DDevice9`，运行期从 `d3d9.dll` 加载），把着色器天花板从 `ps_1_4` 抬向 `ps_3_0`，为多灯、HDR 与 Bloom 效果铺路，同时不改动固定管线行为与回放兼容性。
- 🖱️ **《红警 3》式选中单位构成面板** —— 框选多个单位后，左下角面板会按单位类别分组显示图标与实时数量（例如“炎黄坦克 x2”、“战神坦克 x2”）；点击某个类别图标即可一键全选该类别全部单位，让大规模混编部队的指挥变得格外轻松——代码量很小，却是指挥系统一次关键性的易用性提升。

---

## 🔭 进行中与路线图

我们正在构建以及下一步计划的内容：

1. 🌿 **世界生态系统** —— 用"有生命的"系统丰富场景环境，目前正在进行**草丛 / 植被系统**，并计划升级**粒子特效**（烟雾、尘土、爆炸、天气）。
2. 🤖 **AI 驱动的对手** —— 引入**由人工智能驱动的战斗 AI**，为遭遇战及更多场景带来更丰富的 AI 个性与打法风格。
3. ✨ **新一代渲染特效** —— 在 Direct3D9 端口已交付的基础上，构建它解锁的多灯前向着色、HDR 与高质量 Bloom pass，并以**原生 Vulkan / D3D11 后端**作为长期目标。
4. 🔊 **音视频后端现代化** —— 通过切换到 **MiniAudio / OpenAL（x64）** 音频与 **FFmpeg** 播放过场动画，完成 64 位平台层的收尾。

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

## 💖 参与贡献

欢迎贡献、Bug 反馈与点子 —— 请在提交 issue 或 pull request 前先阅读 [`CONTRIBUTING.md`](CONTRIBUTING.md) 与 [`AGENTS.md`](AGENTS.md) 中的相关规范。

## 📄 许可

详见 [LICENSE](./LICENSE.md)（GPLv3）。
*EA 并未认可亦不支持本产品。Command & Conquer 是 Electronic Arts Inc. 的商标。*
