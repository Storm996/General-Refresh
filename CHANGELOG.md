# 📰 Changelog · 更新日志

本项目所有值得注意的变更都按日期记录在此文件，GitHub 网页上可直接预览。
All notable changes to this project are tracked here by date.

---

## 2026-10-13

### ✨ 新增 / Added

- 🖱️ **《红警 3》式选中单位构成面板** —— 框选多个单位后，左下角面板按单位类别分组显示图标与实时数量（如"炎黄坦克 x2"、"战神坦克 x2"），点击某个类别图标即可一键全选该类别全部单位。
  *RA3-style unit selection panel — grouped per-type icons with live counts; one click selects every unit of that type.*
- 🎇 **PS 3.0 着色器阶梯** —— 像素着色器天花板从 `ps_1_4` 拓宽到 `ps_3_0`，新增着色器模型分级与选项菜单（Options → Shader Model 下拉框）接入。
  *PS 3.0 shader-tier ladder — pixel-shader ceiling widened from `ps_1_4` to `ps_3_0`, with a Shader Model dropdown in Options.*
- ✨ **光影升级（已完成，待发布）** —— 法线贴图、阴影柔化、Bloom 与逐像素多光源均已实现于开发分支，尚未合入主线。
  *Lighting upgrade (done, awaiting release) — normal maps, softened shadows, bloom and per-pixel multi-light shading are implemented on the development branch.*

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
