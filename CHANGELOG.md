# 📰 Changelog · 更新日志

本项目所有值得注意的变更都按日期记录在此文件，GitHub 网页上可直接预览。
All notable changes to this project are tracked here by date.

---

## 2026-10-28

### ✨ 新增 / Added

- 🎞️ **后期分级 / 胶片颗粒 / 暗角（光影升级 2.0 · A1）** —— 在后处理调色分支里加入暗角 `1-smoothstep(0.45,1.20,r)`（强度 0.55）、按帧计时的胶片颗粒（±3%）、饱和度 1.12 与对比度 1.06；只作用于 3D 画面，UI 像素不变。五个不同场景验收全帧 −8.5% / −12.1% / −5.5% / −10.6% / −5.6%，达到 3% 判定门槛。开关沿用高级页“电影调色”。
  *Post grading / film grain / vignette (2.0 · A1) — scene-only grade branch with a 0.55 vignette, frame-timed ±3% grain, 1.12 saturation and 1.06 contrast; UI pixels untouched. Five-scene A/B: −8.5 / −12.1 / −5.5 / −10.6 / −5.6 %. Switch: existing Advanced "Film Grade" row.*

- 💥 **GPU 战斗粒子管线** —— 自建 compute + SSBO 模拟 + `vkCmdDrawIndirect` 的粒子通道，以零售加法批次作为 spawn 源并按批次采样原贴图，火焰形状来自零售 alpha。三段证据（脚手架 / compute / 零售对接）均齐备；默认关闭，ini、启动器、环境变量三处开关。
  *GPU combat particles — a self-built compute + SSBO + indirect pipeline spawned from retail additive batches with per-batch texture sampling. Default off; ini / launcher / env switches.*

- 🌧️ **GPU 天气（雨 / 雪 / 沙尘暴）** —— 复用粒子通道，相机相对盒内环绕，三种天气各自肉眼可见且被地形正常遮挡；开关 `GpuWeather`（1 雨 / 2 雪 / 3 沙尘）。默认关闭。
  *GPU weather (rain / snow / sandstorm) — reuses the particle channel in a camera-relative volume; `GpuWeather` 1/2/3. Default off.*

- 🌫️ **软粒子** —— 合成前把场景深度用 `vkCmdCopyImage` 拷进可采样镜像，粒子像素着色器按 `smoothstep(0, band, sceneZ - z)` 压 alpha，消除粒子与地形的硬边。开关 `SoftParticles`，默认关闭。
  *Soft particles — scene depth is copied into a sampleable image, and the particle shader fades alpha by depth difference. `SoftParticles`, default off.*

- 🔥 **火源热浪扭曲** —— 后处理接收最多 4 个来自粒子 spawn 的热源，在屏幕空间做噪声 UV 偏移；默认强度 1.0 很轻，探针 6～10 可见明显撕裂。开关 `HeatHaze`，默认关闭。
  *Heat haze — up to four live fire sources drive a screen-space noise distortion. `HeatHaze`, default off.*

- 🔲 **MSAA 4x 落地** —— apiVersion 提升到 1.2，渲染目标带采样数、RenderPass2 + 颜色/深度 resolve、framebuffer 四视图，UI 通过格式门排除在多重采样之外。开关 `MSAA`（0 / 4），默认关闭；完整验收待差异定位。
  *MSAA 4x — API 1.2, multisampled targets, RenderPass2 colour/depth resolve, four framebuffer views, UI excluded. `MSAA` 0/4, default off.*

- 🎛️ **体积雾进入启动器** —— 高级图形组新增 `VolumetricFog`（0/1/2/3）、密度、高度、起点、太阳强度五行，默认值与调好的观感一致（1 / 40 / 150 / 20 / 240）；引擎侧新增 `OptionPreferences` 读取与 `[VKFOG]` 日志。
  *Volumetric fog in the launcher — tier, density, height, start and sun rows with the tuned defaults, plus `OptionPreferences` plumbing and a `[VKFOG]` log line.*

### 🐞 修复 / Fixed

- ☀️ **物体光照根因：双重 sRGB 解码** —— rig 已经把光照颜色解码过一次，`DX8NormalMap::Apply` 又解码了第二次，导致建筑/载具按 `sun^2.2` 变暗，而直接读 `fpEnv` 的地形却是对的。对象与固定管线路径不再重复解码引擎光色（贴图、顶点色、自发光等作者色仍保留解码），并移除此前为补偿加的 1.8/1.6 增益。同时按用户标定加入**全局太阳 ×1.5**（`GENERALSX_VK_SUN_BOOST`）。
  *Object lighting root cause: the rig decoded light colours once and the normal-map path decoded them again, shading objects with `sun^2.2`. Object and fixed-function paths no longer re-decode engine light colours; the temporary compensation gains were removed. Added a calibrated global sun ×1.5 (`GENERALSX_VK_SUN_BOOST`).*

- 🌗 **阴影抖动 / 缩放消失** —— 阴影拟合盒原本会随相机在 2816～4864 之间自适应跳变，每次换档都要重缩放光图并重对齐原点，这就是平移时的闪烁，也是“放大后阴影消失 / 屏幕边缘没有阴影”的原因。改为**固定 8192 世界单位盒 + texel 对齐**（`GENERALSX_VKSHADOW_FIT_FIXED=0` 可回退对照），1846 帧环绕+缩放验证盒边长恒定。
  *Shadow fit flicker — the adaptive fit jumped between 2816 and 4864 units, rescaling the light map every switch. Now a fixed 8192-unit texel-snapped box; `GENERALSX_VKSHADOW_FIT_FIXED=0` restores the old fit for A/B.*

- 🌅 **HDR 暖色回归** —— 体积雾的线性雾纱在 HDR 下比原版强约 3 倍，把暖色表面全部洗冷。线性雾纱乘 0.30（SDR 分支不动）、T2.6 强制冷天空色默认关闭、雾色改用保持亮度的解码。草地蓝通道回到原版水平，用户确认“这次改动非常明显”。
  *HDR warm-tone regression — the linear fog veil was ~3x the legacy one under HDR and desaturated warm surfaces. The linear veil is scaled by 0.30, the forced cool sky hue defaults off, and fog colour uses the luma-preserving decode.*

- 🖱️ **抗锯齿不再糊 UI** —— 删掉零售的 `AntiAliasing` 选项行与 `Options.ini` 键，抗锯齿只保留一个所有者：高级页的“后处理抗锯齿”，且 3D 场景在 UI 之前 resolve，HUD 不再被糊。
  *Anti-aliasing no longer blurs the HUD — the retail `AntiAliasing` row and its Options.ini key were removed; post AA is the single owner and the scene resolves before the UI.*

---

## 2026-10-27

### ✨ 新增 / Added

- 🎨 **道路 / 树木 / 植被 / 建筑基座 / 桥梁改为逐像素光照** —— 这些表面一直是零售的“烘焙顶点色”路径，只在昼夜/天气切换时重算，太阳扫描时完全不动。现在道路 tag 120 与树木 tag 121 直接绑到逐像素 twin（并显式 `D3DCULL_NONE` 修正道路整片消失），植被把光照从顶点色移到片元（push constant 128→176 B），bib/bridge 绑到道路 twin。最终对照原版：白天 +4.0%、黄昏 +8.0%、夜间 −0.2%。
  *Per-pixel roads, trees, vegetation, bibs and bridges — these used baked vertex lighting that ignored the moving sun. They now bind to the per-pixel twins; final A/B vs retail is +4.0 % day, +8.0 % dusk, −0.2 % night.*

### 🐞 修复 / Fixed

- 🌈 **HDR 四个缺陷根因** —— ① `gx_SrgbToLinear()` 误实现为 sRGB **OETF**（0.5→0.735）而非 EOTF（0.5→0.214），每次“解码”其实在提亮；② 逐灯线性相加无法复现零售的 gamma 空间求和，地面族改为在显示空间求和后**只解码一次**；③ 太阳阴影接收端把显示空间可见度写进线性乘算，0.42 被当成 0.66；④ 大气内散射颜色被当成光照目标做了 `RelightColor`，改为 sRGB 解码。ACES 预曝光 0.65→0.50 重标定。
  *Four HDR defects — the sRGB decode implemented the OETF instead of the EOTF; per-light linear sum could not reproduce the retail gamma-space sum; the shadow receiver wrote display-space visibility into a linear multiply; the atmosphere in-scatter colour went through a light transform. Fitted-ACES pre-exposure recalibrated 0.65 → 0.50.*

- 🌊 **水面 PVSHADER 反射图从未注册** —— `renderMirror` 渲染出的反射图没有注册给渲染目标记录器，波面水一直采样一张从未写入的图。现以同尺寸深度目标创建并在 adopt 前注册颜色+深度。
  *Water PVSHADER reflection was never registered — the mirror image was rendered but dropped by the target recorder, so wave water sampled an image that was never written. It is now created with a matching depth target and registered before adoption.*

- 🔆 **CAS 在线性 HDR 分支的截断** —— `casSharp()` 原本 clamp 到 0..1，把 tonemap 前的线性高光切掉；线性分支改为 16.0 上限（SDR 仍为 0..1，锐化默认 0 所以默认帧不变）。
  *CAS clamping in the linear branch — `casSharp()` clamped to 0..1 and clipped pre-tonemap linear highlights; the linear branch now clamps at 16.0.*

---

## 2026-10-26

### ✨ 新增 / Added

- 🌫️ **四档体积雾** —— 第 0 档保持零售距离雾；第 1 档沿视线做解析式指数高度积分；第 2/3 档再叠加 8/16 步太阳阴影可见性平均（体积光柱）。默认第 1 档，`GENERALSX_VK_FOG=0` 精确回到原版路径。
  *Four-tier volumetric fog — tier 0 keeps retail distance haze, tier 1 an analytic exponential height integral, tiers 2/3 add 8/16 sun-shadow visibility samples. Default tier 1; `GENERALSX_VK_FOG=0` restores retail exactly.*

- 📖 **体积雾实现与排障手册** —— 调用链、file:line 锚点、六条调试通道、十二个坑位、分诊树与复现命令。
  *Volumetric fog handbook — call chain, file:line anchors, six debug channels, twelve pitfalls and a triage tree.*

### 🐞 修复 / Fixed

- 体积雾接入期修掉六个根因：引擎是 Z-up（首版用了 y）、闭式积分漏掉 `L/dy`、HG 相位未按 `4π` 归一、地形作为 caster 让整层雾都被判为阴影内（改为对比接收点的贴图空间地面深度）、83 m 单元的 ±45% 噪声读成方块（改为 50 m / ±18%）、以及默认参数仅 0.05 的累积量。
  *Six bring-up root causes: Z-up axis, missing `L/dy` term, unnormalised HG phase, terrain casters making the whole layer read as shadowed, tile artefacts from ±45 % noise, and defaults that accumulated only 0.05.*

---

## 2026-10-25

### ✨ 新增 / Added

- 🚀 **外部启动器 v0.1** —— 用 WPF 把“游戏前 UI”从引擎 WND/C++ 菜单里分离出来，包含设置页与图形高级项；引擎增加 `-nomenu` / `-screen` 以支持由启动器拉起。
  *External launcher v0.1 — a WPF shell that separates the pre-game UI from the engine, with a settings page; the engine gained `-nomenu` / `-screen` so the launcher can drive it.*

---

## 2026-10-24

### ✨ 新增 / Added

- 🧩 **帧图接管 GPU 生命周期（6B）** —— `W3DRenderGraph` 接管场景颜色/深度、overlay 目标、五级 Bloom 链与水面反射图；`Require()` 在帧内无 pass 时录制真实的 `vkCmdPipelineBarrier`；`VKDraw_Ready()` 为假时 `Reset()` 目标记录，设备丢失/重建后不再用陈旧句柄。树/草/天空的 PSO 身份改为 material-keyed（PSO 数 50→59，预算 4096）。
  *Frame graph owns GPU lifetime (6B) — scene colour/depth, overlay, the five-level bloom chain and the water reflection image are graph-owned; real `vkCmdPipelineBarrier` recording; target records reset after device loss. Tree/grass/sky PSO identity is material-keyed.*

- 🎞️ **P6 后处理链** —— 场景渲染到 `R16G16B16A16_SFLOAT` HDR 目标；ACES / Reinhard 色调映射、曝光 EV、五级 Bloom、CAS、自动曝光、生成式胶片 LUT、SSAA 100/125/150/200 %、FXAA（仅 3D）；overlay 常开，UI 永不进后处理。
  *P6 post chain — HDR scene target, ACES/Reinhard tone mapping, exposure EV, five-level bloom, CAS, auto exposure, generated film LUT, SSAA and 3D-only FXAA; the overlay is always on so the UI never enters post.*

- 💡 **光照重构 P0–P4 主体** —— 光照数据迁到 SSBO（旧 uniform 块 90,144 B 超过 NVIDIA 64 KB 上限）、`fpLights[512]` 布局修正与 15 个着色器重烘、布局断言与设备上限守卫；`lighting_common.glsl` 的 GGX、半球环境光、深度 AO（默认 0.50）、阴影只减直射、逆平方点光衰减。
  *Lighting refactor P0–P4 — lighting moved to SSBOs (the old 90,144 B uniform block exceeded NVIDIA's 64 KB limit), `fpLights[512]` layout fixes and 15 re-baked shaders, GGX in `lighting_common.glsl`, hemisphere ambient, depth AO (0.50), direct-only shadows and inverse-square point falloff.*

- 🔦 **P13–P15 点光质量与 QA** —— 灯按强度有界入选（预算 128，300 灯压测簇光档 156.9 fps / 直接档 120.0 fps）、逆平方参考距离改为 `max(p.w, 0.35×reach)` 恢复夜间灯池、锥光 + 程序化窗格 cookie 通道、5 个标定固定机位与 golden 截图工具链。
  *P13–P15 point-light quality and QA — strength-bounded light admission (budget 128; 300 lights at 156.9 fps clustered / 120.0 fps direct), corrected inverse-square reference distance, spot cones with procedural gobos, five calibrated fixed cameras and golden screenshots.*

- 🎨 **共享光照 / 材质体系** —— `W3DRenderLightingState`（太阳/环境/雾/高度衰减/云影/曝光/revision）、地形/静态/皮肤材质实例、水面/草/天空描述符绑定、`W3DMaterialPipelineKey`，以及独立的 5 面 cubemap 天空渲染器。
  *Shared lighting/material system — `W3DRenderLightingState`, terrain/static/skin material instances, water/grass/sky binders, `W3DMaterialPipelineKey` and an independent five-face cubemap sky renderer.*

### 🐞 修复 / Fixed

- 🖤 **局部 3D 黑屏** —— 部分遭遇战/全屏场景 3D 全黑，用户实机确认修复（修复窗口与目标生命周期、预帧烘焙跳过、D24S8 深度格式这组改动重叠）。
  *Local 3D black screen — fixed and confirmed on hardware; the fix window overlaps the target lifetime, pre-frame bake skip and D24S8 depth-format changes.*

- 🪟 **主菜单黑屏** —— 根因是陈旧对象引用，不是渲染器问题。
  *Main-menu black screen — stale object references, not a renderer bug.*

- 🌳 **灌木贴图闪烁与拉伸** —— 树/灌木的动态顶点/索引缓冲在 Vulkan 端按 `dynamic=0` 每帧原地重写，3 帧在飞导致读到半新半旧数据。改为按 `USAGE_DYNAMIC` 传 `dynamic=1` 按帧分槽，树缓冲推挤/倒伏改为全量重建（约 +35 MB host-visible）。
  *Bush flicker and stretched triangles — dynamic tree/bush buffers were rewritten in place with `dynamic=0` while three frames were in flight; now per-frame slots with `dynamic=1` and full rebuilds for push/fall.*

---

## 2026-10-23

### ✨ 新增 / Added

- 🎇 **渲染后端整体切换到原生 Vulkan** —— W3D 绘制核心改为自建 Vulkan 后端（渲染目标、命令缓冲、描述符、管线缓存），运行期默认**不加载任何 Direct3D 运行库**，旧的设备 API 由内部桩件承接；上一阶段的 Direct3D9 渲染路径正式退役。
  *The renderer moved to a native Vulkan backend — self-managed render targets, command buffers, descriptors and pipeline cache. No Direct3D runtime is loaded by default and the previous Direct3D9 path is retired.*

- 💡 **Forward+ 簇光光照** —— 灯光块 SSBO、簇光与直接两档、点光互动与夜景验收；灯块按簇分配，支持 512 个灯槽。
  *Forward+ clustered lighting — light-block SSBO, clustered and direct paths, point-light interaction and night-scene acceptance, 512 light slots.*

- 📐 **光照重构立项** —— 44 条问题 → 41 个任务 → 6 个 Gate 的全量技术路线，含预期现象、覆盖率自检与排期；同时建立 5 档 QA 机位与差异对比工具。
  *Lighting refactor kick-off — 44 problems mapped to 41 tasks and 6 gates, with coverage self-checks, QA cameras and diff tooling.*

- 🧱 **渲染架构统一化（1–5A / 5B / 6A）** —— 树、草、水面统一到材质实例与绑定器，删除 TREE30 特殊路径与 `setupJbaWaterShader()`；天空改为独立渲染器并在 world pass 后无条件执行；清理 Sky 五面 mesh 渲染 fallback。
  *Renderer unification (1–5A/5B/6A) — trees, grass and water unified into material instances/binders, the TREE30 special path and `setupJbaWaterShader()` removed, an independent sky renderer executed after the world pass, and the five-face mesh sky fallback deleted.*

---

## 2026-10-15

### ✨ 新增 / Added

- 🎨 **光影升级正式交付** —— `feat/lighting-upgrade` 合入主线：法线贴图（dot3）、基于物理的材质（PBR 粗糙度 / 金属度）、阴影柔化、纹理过滤（默认各向异性 4x）、水面与地形渲染打磨一并发布。
  *Lighting upgrade shipped — `feat/lighting-upgrade` merged to main: dot3 normal maps, PBR roughness / metalness, softened shadows, anisotropic filtering (4x by default), water and terrain rendering polish.*
- 🔩 **PBR 粗糙度实装** —— 金属-粗糙度 PBR 路径接入 dot3 光照管线：`GENERALSX_PBR=1` 选 rough-Blinn 变体、`=2` 选 GGX 变体；缺少 `_rp` 材质图的表面回退到常数粗糙 / 金属值。资产侧由 AI 贴图分解管线（CHORD）把 diffuse 分解为 basecolor / 法线 / 粗糙度 / 金属度四通道，打包为引擎 `_rp.tga` 消费图（R=粗糙度、G=金属度），实机验证 `[PBR] variant compiled/installed/mounted` 全链路成立，回放 CRC 对照证明与逻辑无关。
  *PBR roughness shipped — metallic-roughness PBR wired into the dot3 pipeline: `GENERALSX_PBR=1` picks the rough-Blinn variant, `=2` the GGX variant; surfaces missing `_rp` fall back to constant rough/metal values. Assets come from an AI texture-decomposition pipeline (CHORD) producing basecolor / normal / roughness / metalness channels packed into engine `_rp.tga` maps (R=roughness, G=metalness), verified on hardware with `[PBR] variant compiled/installed/mounted` and replay-CRC-proven logic independence.*

## 2026-10-14

### ✨ 新增 / Added

- 💡 **逐像素光照（L2，几何法线 Gouraud / Blinn-Phong）** —— 战场单位光照从逐顶点升级为逐像素：读取批次自身的光照环境（最多 4 盏方向光），用世界空间几何法线逐像素计算漫反射与 Blinn-Phong 高光，明暗交界更锐利；默认开启，`GENERALSX_GEOMNORMAL=0` 回退逐顶点。
  *Per-pixel lighting (L2, geometric-normal Gouraud / Blinn-Phong) — battlefield lighting upgraded from per-vertex to per-pixel: reads each batch's own light environment (up to 4 directional lights) and evaluates diffuse plus Blinn-Phong specular per pixel with world-space geometric normals; enabled by default, `GENERALSX_GEOMNORMAL=0` falls back to per-vertex.*
- 🎛️ **出厂光照定案（L1）** —— 环境底光、冷色天空补光与太阳主光增益旋钮（`GENERALSX_KEY_GAIN`）统一到一处 rig。
  *Shipping light rig (L1) — ambient floor, sky fill light and a sun gain knob (`GENERALSX_KEY_GAIN`) consolidated into one rig.*
- 📷 **截屏链路修复** —— MSAA 下改用前置缓冲抓取，截图包含完整 HUD。
  *Screenshot path fix — switched to front-buffer capture so screenshots include the full HUD.*
- 🤖 **AI 风格化驱动升级** —— 遭遇战 AI 在难度层之上新增 8 种可选个性风格（进攻 / 防守 / 支援 / Rush / 闪击 / 人海 / 铁壁 / 潜入）外加神经 AI 脚手架（P0 通路：观测编码器、动作解码器、占位策略），每个 AI 槽位可独立选择；残暴难度同步强化（组队节奏减半、集结窗口 60s→45s）。
  *AI personality-style drivers — Skirmish AI gains 8 selectable styles (Offensive / Defensive / Support / Rush / Blitz / Horde / Fortress / Sneak) plus a neural-AI scaffold (P0 path: observation encoder, action decoder, placeholder policy) on top of the difficulty layer, selectable per AI slot; brutal difficulty strengthened (halved team tempo, rally window 60s→45s).*
- 🧪 **AI 取证与自动进局工具链** —— `GENERALX_AI_TRACE=1` 取证通道记录驱动分派 / 队伍激活 / 防御工事事件；`GENERALX_QA_SKIRMISH` 无头自动进遭遇战钩子；8 风格 A/B 实测全链路 PASS。
  *AI trace & QA tooling — `GENERALX_AI_TRACE=1` records driver dispatch / team activation / fortification events; `GENERALX_QA_SKIRMISH` headless skirmish hook; 8-style A/B verified PASS on hardware.*

## 2026-10-13

### ✨ 新增 / Added

- 🖱️ **《红警 3》式选中单位构成面板** —— 框选多个单位后，左下角面板按单位类别分组显示图标与实时数量（如“炎黄坦克 x2”、“战神坦克 x2”），点击某个类别图标即可一键全选该类别全部单位。
  *RA3-style unit selection panel — grouped per-type icons with live counts; one click selects every unit of that type.*

## 2026-10-12

### ✨ 新增 / Added

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
