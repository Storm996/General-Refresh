<p align="center">
  <img src="assets/general-refresh_icon.png" alt="General Refresh 图标" width="128" height="128">
</p>

<h1 align="center">General Refresh</h1>

<p align="center">
  <strong>基于 <a href="https://github.com/fbraz3/GeneralsX">GeneralsX</a> 的《命令与征服：将军 / 绝命时刻》现代化增强版 —— 原生 Vulkan 渲染器、Forward+ 簇光光照、物理 HDR 后处理管线、AI 高清贴图，以及真正流畅的高帧率游戏体验。</strong>
</p>

<p align="center">
  <a href="https://github.com/Storm996/General-Refresh/releases"><img src="https://img.shields.io/github/v/release/Storm996/General-Refresh?include_prereleases&sort=date&display_name=tag&style=flat&label=Release" alt="GitHub 版本"></a>
  <a href="https://github.com/fbraz3/GeneralsX"><img src="https://img.shields.io/badge/基于-GeneralsX-blueviolet" alt="基于 GeneralsX"></a>
  <img src="https://img.shields.io/badge/渲染器-Vulkan%20(原生)-red" alt="原生 Vulkan 渲染器">
  <img src="https://img.shields.io/badge/光照-Forward%2B%20%C2%B7%20512%20%E7%81%AF-orange" alt="Forward+ 光照">
  <img src="https://img.shields.io/badge/后处理-HDR%20%C2%B7%20ACES%20%C2%B7%20Bloom-cyan" alt="HDR 后处理">
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

GeneralsX 完成了**平台层**的现代化：窗口、64 位工具链、现代音视频后端与跨平台联机——让游戏能在今天的系统上运行。**General Refresh 则更进一步：把渲染器重写、把光照重做。** 确定性模拟一行不动，玩家体验真正往前推进：

### 🎨 画质优化

战场由**原生 Vulkan 渲染器**绘制——Forward+ 簇光光照、统一逐像素光照 rig、太阳与动态灯光阴影图、体积雾，以及一条包含 ACES 色调映射、Bloom 与电影调色的 HDR 后处理链。配合 AI 精修贴图、dot3 法线贴图与 PBR 粗糙度材质，2003 年的固定管线观感已经彻底翻篇。

### 🕹️ 游戏特性与体验

高帧率战场、红警 3 式选中面板、任务界面改版、主菜单直进任意任务，以及 8 种风格的 AI 驱动。

### 🛠️ 平台与构建

**自建 Vulkan 后端**（运行期完全不加载 Direct3D 运行库）、原生 64 位构建、现代音视频后端（MiniAudio / OpenAL + FFmpeg），以及一个独立的外部启动器来暴露全部图形设置。

> *说明：与 GeneralsX 一样，本项目是引擎重制，需要《命令与征服：将军 / 绝命时刻》正版副本的原始游戏数据。*

---

## ✅ 已交付的特性

以下内容**均已实现并可玩**：

### 🖼️ 渲染器与架构

- 🎇 **原生 Vulkan 渲染器** —— W3D 绘制核心跑在自建的 Vulkan 后端上（渲染目标、命令缓冲、描述符集、管线缓存全部自管）。运行期不加载任何 Direct3D 运行库：旧的设备 API 由内部桩件承接，着色器一律预烘为 GLSL → SPIR-V。整条管线已经不存在固定管线的天花板，全部是可编程的。
- 🧩 **帧图（Frame Graph）** —— `W3DRenderGraph` 接管场景颜色/深度、overlay 目标、五级 Bloom 链与水面反射图的 GPU 生命周期，并录制真实的 `vkCmdPipelineBarrier` 转换；设备丢失后目标自动重建。
- 💡 **Forward+ 簇光光照** —— 光照数据迁移到 SSBO（旧的 uniform 块在 NVIDIA 上超过 64 KB 上限），提供 **512 个灯槽**（`fpLights[512]` + `fpLightExtra[512]`）；簇光与直接（256）两条路径都能跑，每帧预算默认 128 盏并按强度有界入选。300 灯 / 1080p 压测：簇光档 ≈ 157 fps、直接档 120 fps。
- 🎨 **统一光照 rig** —— 单一 `W3DRenderLightingState`（太阳、环境、雾、高度衰减、云影、曝光、revision）同时供给地形、道路、树木、植被、建筑基座、桥梁、水面、对象与固定管线路径。昼夜与天气因此对所有表面一致生效，大气变化会自动重烘受影响的批次。
- ☀️ **GGX 太阳、半球环境光、深度 AO、阴影只减直射** —— 地形、道路、对象与点光都有逐像素 GGX 高光；环境光改为天空 + 地面半球项而非一个平的环境色；深度 AO（默认 0.50）；阴影图只衰减太阳直射，环境光与自发光仍能撑住暗部。
- 🌗 **阴影图** —— 4096² 太阳阴影图，采用**固定 8192 世界单位盒 + texel 对齐**（消除平移时的阴影闪烁与“缩放后阴影消失”），地形与道路参与投射，同一帧最多 **4 张动态灯影图**，带软过滤与云影。
- 💡 **点光与锥光** —— 逆平方衰减（参考距离已修正）、带内外角的真实锥光、8 格灯光 cookie 图集 + 程序化窗格 gobo、车灯锥，以及枪口闪光 / 命中光脉冲。
- 🌫️ **体积雾（四档）** —— 第 0 档是原版距离雾，第 1 档沿视线做解析式指数高度积分，第 2/3 档再叠加 8/16 步太阳阴影可见性采样（体积光柱感）。密度、高度、起点距离与太阳强度都可在启动器中调节。
- 🎞️ **HDR 后处理管线** —— 场景渲染到 `R16G16B16A16_SFLOAT`，随后是 ACES/Reinhard 色调映射 + 曝光 EV、五级 Bloom、CAS 锐化、自动曝光、生成式胶片 LUT、SSAA 100/125/150/200%、FXAA（仅 3D）以及可选的电影调色（暗角、颗粒、饱和、对比）。UI 永远在 resolve **之后**合成，后处理不会把 HUD 糊掉或染色。
- 🖌️ **材质** —— 切线空间 dot3 法线贴图、金属-粗糙度 PBR（rough-Blinn 与 GGX 两种变体），以及一条 AI 贴图分解管线（CHORD）产出 `_nm` / `_rp` 伴生图；地形法线图集改为单层，修掉了图集接缝。
- 🌿 **世界表面** —— 道路、树木、植被、建筑基座与桥梁全部改为逐像素着色（不再用无视太阳的烘焙顶点光），海面 / 河流 / 梯形水面共用一套材质实例并支持天空反射与 Forward+ 点光，天空改为独立的 5 面 cubemap 渲染器，草丛与植被布点重做。
- 🎨 **AI 高清贴图** —— 单位与地形贴图经 AI 辅助管线重绘与放大，替换掉模糊的原版贴图。

### 🕹️ 游戏特性与体验

- 🏎️ **高帧率游戏体验** —— 战场本身如今跑得顺畅：单位位移、转向、炮塔瞄准、开火与动画都在遭遇战、战役与电影镜头下按显示刷新率插值——全部是建立在不变确定性模拟之上的表现层改进，因此联机、局域网与回放保持逐位忠实。
- 🖱️ **《红警 3》式选中单位构成面板** —— 框选多个单位后，左下角面板会按单位类别分组显示图标与实时数量（例如“炎黄坦克 x2”、“战神坦克 x2”）；点击某个类别图标即可一键全选该类别全部单位，让大规模混编部队的指挥变得格外轻松。
- 🧩 **任务界面改版** —— 任务简报 / 选择界面经过重新设计。
- 🎬 **直接进入任意任务** —— 可在**主菜单直接选择任意任务**并立即进入，无需沿整条战役进度一路推进。
- 🤖 **AI 风格化驱动升级** —— 遭遇战 AI 在难度层之上新增 8 种可选个性风格（进攻、防守、支援、疯狗 Rush、闪击、人海、铁壁、老六潜入）外加神经 AI 脚手架，每个 AI 槽位可独立选择；残暴难度同步强化（组队节奏减半、集结窗口提前），`GENERALX_AI_TRACE=1` 取证通道可记录驱动分派、队伍激活与防御工事事件。

### 🛠️ 平台与构建

- 🖥️ **原生 64 位（Windows x64）构建** —— Windows 客户端运行在 64 位工具链之上，彻底突破遗留 32 位进程的 2GB/4GB 地址空间墙，**真正用满可用的系统内存与物理显存**。
- 🔊 **音视频后端现代化** —— 音频由 **MiniAudio / OpenAL（x64）** 驱动，取代 32 位 Miles 引擎；开场视频与过场动画由 **FFmpeg** 解码播放。
- 🚀 **外部启动器** —— 一个 WPF 启动器把"游戏前 UI"从引擎里分离出来，并暴露整套图形设置：分辨率与 LOD、灯预算、体积雾（档位/密度/高度/起点/太阳）、HDR、色调映射、Bloom、AO、后处理抗锯齿、MSAA、GPU 粒子与天气、电影调色，以及高级页的其余选项。
- 🧪 **QA 工具链** —— 5 个标定过的固定机位、golden 截图、PNG 差异对比工具、着色器烘焙与光照布局自检、回放 CRC 验证，每一项画质改动都能对照原版基线取证。

### 🧪 已实现但默认关闭

以下功能在出货后端上已经跑通，但**默认关闭**，可在启动器 / Options.ini 中打开：

- 💥 **GPU 战斗粒子** —— compute + SSBO 模拟驱动 `vkCmdDrawIndirect`，以零售加法批次作为 spawn 源（保留原贴图）。
- 🌧️ **GPU 天气** —— 雨、雪、沙尘暴三种相机相对粒子体积。
- 🌫️ **软粒子** 与 🔥 **热浪扭曲** —— 场景深度淡出，以及由实时火焰驱动的屏幕空间扰动。
- 🔲 **MSAA 4x** —— 颜色 + 深度 resolve 已跑通，alpha-to-coverage 为下一步；UI 不参与多重采样。
- 🌟 **神光/太阳轴** —— 实现与投影均已验证正确，只是 RTS 俯视机位下太阳本来就在画面外，因此默认保持关闭，仅在低角度视角需要时开启。

---

## 🔭 进行中与路线图

我们正在构建以及下一步计划的内容：

1. 🌊 **屏幕空间反射（SSR）** —— 需要先有 G-Buffer 法线/高度趟；当前后处理靠深度重建法线，掠射面上噪点过大。
2. 🌤️ **预过滤 IBL** —— GGX 预过滤的天空 cubemap（逐 mip 卷积），并在反射接口中加入粗糙度通道。
3. ☁️ **物理天空 + 空气透视** —— 用程序化大气替换缝合式 cubemap。
4. 🔲 **TAA / SMAA** —— 两者都需要额外缓冲或 LUT 素材；叶片与边缘闪烁预期先用 MSAA + alpha-to-coverage 解决更划算。
5. 🧱 **HBAO / GTAO** 与 **贴花参与光照** —— 与 SSR 卡在同一份 G-Buffer 前置上。
6. 🌿 **世界生态系统** —— 更丰富的植被，以及更长线的粒子特效升级（烟雾、尘土、爆炸、天气）。
7. 🗺️ **任务地图支持** —— 扩展对任务 / 自定义地图的处理。

---

## 📰 更新日志

每一次更新都按日期记录在 **[CHANGELOG.md](CHANGELOG.md)** 中——从初始导入、64 位构建，到 Vulkan 渲染器重写、光照重构与 2.0 画质专项，哪一天交付了什么一目了然。

---

## 🚀 快速上手

### 下载与游玩

可直接运行的构建已发布于版本页面：

* 📦 **[下载 General Refresh 版本](https://github.com/Storm996/General-Refresh/releases)**
* 📖 安装与游戏数据配置指南可参阅上游 Wiki：[GeneralsX Wiki](https://github.com/fbraz3/GeneralsX/wiki)

### 🛠️ 从源码构建

General Refresh 使用 CMake presets 构建。Windows 上只有一个受支持的入口——**Vulkan core** 构建：

```bat
build-win64-vk.bat
:: 产物：build\win64vk\GeneralsMD\Release\GeneralRefresh.exe

deploy-win64-vk.bat
:: 把 exe 部署到游戏目录（会先备份旧 exe）
```

基础版（将军）用同一个脚本加 `-Game Generals` 开关，构建到 `build\win64vk-generals` → `GeneralsX.exe`。preset 与启动器都会在 CMake 缓存不是 Vulkan-core 时直接报错退出。

跨平台的 Linux / macOS 流水线，请参阅 [GeneralsX 构建指南](https://github.com/fbraz3/GeneralsX/wiki)。

---

## 📜 项目血脉与致谢

General Refresh 站立在 RTS 社区与开源先驱的肩膀之上：

* **[Westwood Studios 与 EA](https://www.ea.com/)** —— 创作了《命令与征服：将军》的世界。
* **[TheSuperHackers](https://github.com/TheSuperHackers/GeneralsGameCode)** —— 基础的代码现代化、稳定性修复与上游保存。
* **[GeneralsX / fbraz3](https://github.com/fbraz3/GeneralsX)** —— 本项目直接 fork 自的跨平台重制引擎。
* **[Fighter19](https://github.com/Fighter19) 与 [feliwir](https://github.com/feliwir)** —— 率先在《绝命时刻》中引入现代平台与图形层的参考性工作。

---

## 📄 许可

详见 [LICENSE](./LICENSE.md)（GPLv3）。
*EA 并未认可亦不支持本产品。Command & Conquer 是 Electronic Arts Inc. 的商标。*
