# 📰 Changelog · 更新日志

本项目所有值得注意的变更都按日期记录在此文件，GitHub 网页上可直接预览。
All notable changes to this project are tracked here by date.

---

## 2026-10-15

### ✨ 新增 / Added

- ✨ **光影升级正式交付** —— `feat/lighting-upgrade` 合入主线：法线贴图（dot3）、基于物理的材质（PBR 粗糙度 / 金属度）、阴影柔化、纹理过滤（默认各向异性 4x）、水面与地形渲染打磨一并发布。
  *Lighting upgrade shipped — `feat/lighting-upgrade` merged to main: dot3 normal maps, PBR roughness / metalness, softened shadows, anisotropic filtering (4x by default), water and terrain rendering polish.*
- 🔩 **PBR 粗糙度实装** —— 金属-粗糙度 PBR 路径接入 dot3 光照管线：`GENERALSX_PBR=1` 选 rough-Blinn 变体、`=2` 选 GGX 变体；缺少 `_rp` 材质图的表面回退到常数粗糙 / 金属值。资产侧由 AI 贴图分解管线（CHORD）把 diffuse 分解为 basecolor / 法线 / 粗糙度 / 金属度四通道，打包为引擎 `_rp.tga` 消费图（R=粗糙度、G=金属度），实机验证 `[PBR] variant compiled/installed/mounted` 全链路成立，回放 CRC 对照证明与逻辑无关。
  *PBR roughness shipped — metallic-roughness PBR wired into the dot3 pipeline: `GENERALSX_PBR=1` picks the rough-Blinn variant, `=2` the GGX variant; surfaces missing `_rp` fall back to constant rough/metal values. Assets come from an AI texture-decomposition pipeline (CHORD) producing basecolor / normal / roughness / metalness channels packed into engine `_rp.tga` maps (R=roughness, G=metalness), verified on hardware with `[PBR] variant compiled/installed/mounted` and replay-CRC-proven logic independence.*

## 2026-10-14

### ✨ 新增 / Added

- 💡 **逐像素光照（L2，几何法线 Gouraud / Blinn-Phong）** —— 战场单位光照从逐顶点升级为逐像素：读取批次自身的光照环境（最多 4 盏方向光），用世界空间几何法线逐像素计算漫反射与 Blinn-Phong 高光，明暗交界更锐利；默认开启，`GENERALSX_GEOMNORMAL=0` 回退逐顶点。
  *Per-pixel lighting (L2, geometric-normal Gouraud / Blinn-Phong) — battlefield lighting upgraded from per-vertex to per-pixel: reads each batch's own light environment (up to 4 directional lights) and evaluates diffuse plus Blinn-Phong specular per pixel with world-space geometric normals; enabled by default, `GENERALSX_GEOMNORMAL=0` falls back to per-vertex.*
- 🌗 **出厂光照定案（L1）** —— 环境底光默认压至 0.5 增强明暗对比（`GENERALSX_AMBIENT_SCALE` 可调）；冷色天空补光默认关闭（`GENERALSX_FILL_LIGHT=1` 开启）；太阳主光增益旋钮 `GENERALSX_KEY_GAIN`。
  *Shipping light rig (L1) — ambient floor scaled to 0.5 by default for stronger contrast (`GENERALSX_AMBIENT_SCALE`); sky fill light off by default (`GENERALSX_FILL_LIGHT=1` enables); sun gain knob `GENERALSX_KEY_GAIN`.*
- 📸 **截屏链路修复** —— MSAA + DXVK 下改用 `GetFrontBufferData`，截图包含完整 HUD。
  *Screenshot path fix — switched to `GetFrontBufferData` under MSAA + DXVK so screenshots include the full HUD.*
- 🤖 **AI 风格化驱动升级** —— 遭遇战 AI 在难度层之上新增 8 种可选个性风格（进攻 / 防守 / 支援 / Rush / 闪击 / 人海 / 铁壁 / 潜入）外加神经 AI 脚手架（P0 通路：观测编码器、动作解码器、占位策略），每个 AI 槽位可独立选择；残暴难度同步强化（组队节奏减半、集结窗口 60s→45s）。
  *AI personality-style drivers — Skirmish AI gains 8 selectable styles (Offensive / Defensive / Support / Rush / Blitz / Horde / Fortress / Sneak) plus a neural-AI scaffold (P0 path: observation encoder, action decoder, placeholder policy) on top of the difficulty layer, selectable per AI slot; brutal difficulty strengthened (halved team tempo, rally window 60s→45s).*
- 🕵️ **AI 取证与自动进局工具链** —— `GENERALX_AI_TRACE=1` 取证通道记录驱动分派 / 队伍激活 / 防御工事事件；`GENERALX_QA_SKIRMISH` 无头自动进遭遇战钩子；8 风格 A/B 实测全链路 PASS。
  *AI trace & QA tooling — `GENERALX_AI_TRACE=1` records driver dispatch / team activation / fortification events; `GENERALX_QA_SKIRMISH` headless skirmish hook; 8-style A/B verified PASS on hardware.*

## 2026-10-13

### ✨ 新增 / Added

- 🖱️ **《红警 3》式选中单位构成面板** —— 框选多个单位后，左下角面板按单位类别分组显示图标与实时数量（如"炎黄坦克 x2"、"战神坦克 x2"），点击某个类别图标即可一键全选该类别全部单位。
  *RA3-style unit selection panel — grouped per-type icons with live counts; one click selects every unit of that type.*
- 🎇 **PS 3.0 着色器阶梯** —— 像素着色器天花板从 `ps_1_4` 拓宽到 `ps_3_0`，新增着色器模型分级与选项菜单（Options → Shader Model 下拉框）接入。
  *PS 3.0 shader-tier ladder — pixel-shader ceiling widened from `ps_1_4` to `ps_3_0`, with a Shader Model dropdown in Options.*

## 2026-10-12

### ✨ 新增 / Added

- 🎇 **Direct3D 9 渲染器** —— `DX8Wrapper` 核心从 Direct3D 8 完整端口到 Direct3D 9，运行期从 `d3d9.dll` 动态加载；固定管线行为与回放兼容性保持不变。
  *Direct3D9 renderer — `DX8Wrapper` ported from D3D8 to modern D3D9, loaded at runtime; fixed-pipeline behaviour and replay compatibility unchanged.*
- 🖥️ **原生 64 位构建正式交付** —— Windows x64 客户端标记为已交付特性，突破 32 位进程 2GB/4GB 地址空间墙。
  *Native 64-bit build shipped — Windows x64 client promoted to shipped; breaks the 32-bit 2GB/4GB address-space ceiling.*
- 🔊 **音视频后端现代化** —— 随 64 位切换一同完成：音频从 32 位 Miles 转向 **MiniAudio / OpenAL（x64）**，过场动画由 **FFmpeg** 解码播放。
  *Audio & video backend modernization — shipped together with the 64-bit switch: audio moved from 32-bit Miles to MiniAudio / OpenAL (x64), video decoded by FFmpeg.*

## 2026-10-11

### ✨ 初始导入 / Initial import

项目仓库建立，随初始版本一同交付的功能：
The repository was created; these features shipped with the initial import:

- 🏎️ 高帧率游戏体验（单位运动 / 转向 / 开火按显示刷新率插值）
- 🧱 法线贴图（dot3）渲染
- 🎨 AI 辅助贴图重绘 / 超分
- 🧩 任务界面改版
- 🎬 主菜单直接进入任意任务
