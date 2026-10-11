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

---

## 🔭 进行中与路线图

我们正在构建以及下一步计划的内容：

1. 🧮 **原生 64 位编译系统** —— 全面转向 64 位工具链，突破遗留 32 位进程的上限，**释放可用的系统内存与显存**，以支撑更大的地图、高清资源与繁重的粒子负载。
2. 🌿 **世界生态系统** —— 用"有生命的"系统丰富场景环境，目前正在进行**草丛 / 植被系统**，并计划升级**粒子特效**（烟雾、尘土、爆炸、天气）。
3. 🤖 **AI 驱动的对手** —— 引入**由人工智能驱动的战斗 AI**，为遭遇战及更多场景带来更丰富的 AI 个性与打法风格。

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
