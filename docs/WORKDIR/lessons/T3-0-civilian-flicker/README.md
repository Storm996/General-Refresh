# T3-0 Dot3 平民建筑"雪斑"闪烁排查全记录

> 日期：2026-10-10 ~ 2026-10-11
> 范围：T3-0 法线贴图 Dot3 逐像素着色引擎对接（GeneralsX ZH）
> 结论：闪烁源 = 平民建筑（CB 系列）挂载 dot3 的 submesh 渲染；移除其法线贴图后闪烁完全消失。
> 注：本文档由 AI 编码助手基于实际排查会话整理，所有实验结论均来自运行时日志与实机验证。

---

## 一、现象

1. **只有平民建筑闪**（CB 前缀：CBTOWNHS01/02、CBGSHOP、CBGPUB 等），玩家建筑、单位、坦克均正常。
2. 闪的样子"像被雪覆盖，但没覆盖全，只覆盖一部分"——灰白色、有纹理。
3. **移动视角时闪得非常剧烈，静止时轻微**（但静止也闪）。
4. 两栋外形相同的建筑，同一时刻一栋像"闪前"、一栋像"闪后"。
5. 多次修 shader（光照方向、TBN、归一化、mip）均无法消除。

## 二、排查时间线（每轮 = 假设 → 实验 → 结论）

| # | 假设 | 实验 | 结论 |
|---|------|------|------|
| 1 | mip LOD 抖动 | 改用单级 mip DDS | ❌ 还闪，排除 mip |
| 2 | 法线贴图采样问题 | NOSTAGE1=1（不采样法线，常数法线+面光照） | ❌ 还闪，排除 stage1 采样 |
| 3 | 逐面光照随视角变化 | VS 输出常数光（oD1=c4 直通，不随面/视角变） | ❌ 还闪，排除光照计算 |
| 4 | 引擎雪地贴图变体（MODELCONDITION_SNOW） | 在 Drawable 状态切换点打 [SNOWSTATE] 日志 | ❌ 0 条，排除雪地变体 |
| 5 | 贴图帧间切换 | [NM_TEXSWITCH] 检测 | ⚠️ 发现交替但需辨真伪 |
| 6 | 水面镜像 pass 干扰 | [NM_RENDER] 全量日志加 mirror 标志 | ❌ mirror=0 全部，排除镜像 |
| 7 | dot3 挂载不稳定（时挂时不挂） | 按 (mesh,tex,pass,fvf) 分组统计 applied 翻转 | ❌ 0 翻转，挂载完全稳定 |
| 8 | **判别对照：dot3 整体关闭** | GENERALSX_NORMALMAP=0 | ✅ **不闪了** → 闪与 dot3 强相关 |
| 9 | **判别对照：移除平民建筑法线贴图** | 把 CB 建筑 12 组法线贴图（TGA+DDS）移出 Art\Textures | ✅ **完全不闪** → 实锤闪源 |

## 三、根因结论

### 实锤证据链

1. **闪只在 dot3 挂载时出现**（实验 8：dot3 全关 → 不闪）。
2. **CB 建筑里唯一挂 dot3 的是 lakedusk.tga 等 12 组贴图对应的 submesh**：
   - 墙面主体（cbtownhs01.tga 等）**没有配套法线贴图**，从未挂载 dot3（applied=0）；
   - 玻璃/装饰 submesh（lakedusk.tga 等）**有配套法线贴图**，每帧稳定挂载（applied=1）；
   - [NM_RENDER] 日志证实每帧渲染 4 次：2× 玻璃 pass（applied=1）+ 2× 墙面 pass（applied=0），**帧内帧间完全稳定、0 翻转**。
3. **移除这 12 组法线贴图 → 引擎 companion 查找失败 → 玻璃 submesh 退回固定管线 → 闪消失**（实验 9）。

### 推断机制（待进一步二分验证）

- lakedusk.tga 是"黄昏湖"天空/玻璃贴图，**灰白色调**——用户看到的"雪"就是它在 dot3 光照下的表现。
- [NM_RENDER] 显示同一 submesh **每帧渲染 2 遍**（同 tick 两行同参数）；dot3 改变了两遍渲染的输出（暗化+浮雕），两遍叠加/深度竞争产生高频明暗抖动。
- 移动视角时玻璃贴图的 UV 滚动（模拟天空反射）与相机运动叠加 → 闪加剧；静止时仅剩双遍渲染抖动 → 轻微。

## 四、当前修复状态（判别实验期）

- 已移出 12 组 CB 建筑法线贴图（**TGA + DDS 双格式各 12 张**，共 24 个文件）：
  `cbquonset1, cbtoildepo, cbtschlhs, cthole, firescape, lakedusk, pmbarbwire, pmbarbwire2, pmbush01, pmflwrbed01, pmsandbags2, roofparts04`
- 存放位置：`D:\Games\Generals\Command and Conquer Generals Zero Hour\Art\Textures_nm_bak\`（可随时还原）
- 其余法线贴图（2533 张 `_nm.dds` 中的其余部分）未动，坦克/玩家建筑 dot3 正常工作。
- 代码层：`dx8renderer.cpp` hook 已加"仅不透明 mesh 挂载"规则（blend=1/0），但该规则**不是本次闪源的修复**（lakedusk 是不透明 blend，规则排除不掉它）。

## 五、遗留工作

1. **二分还原**：12 组贴图中可能只有 1~2 组是闪源（高度怀疑 lakedusk）。逐组放回法线贴图 → 重启 → 观察闪，锁定精确贴图，恢复无辜贴图（灌木、沙袋等法线效果应保留）。
2. **代码层修复方向**：若确认闪源是"天空/玻璃反射类贴图 + dot3"，考虑按贴图语义排除（如 sky/glass 前缀），或排查"每帧渲染 2 遍"的根因（为何同名 mesh 同 tick 生成 2 个 render task）。
3. **清理诊断代码**：`dx8renderer.cpp` 的 [NM_RENDER] 全量日志已移除（性能坑）；`Drawable.cpp` 的 [SNOWSTATE] 日志在收尾时移除。

## 五-B、第二阶段（建筑效果增强与资产补齐，2026-10-11）

### 建筑均等变黑（ps.1.4 swizzle 陷阱）
- 现象：XY 放大 2.5 后建筑均等变黑（正光背光都暗、贴图细节仍在），兵营不黑（它当时没挂）。
- 根因：`mad r3.z, r1, c3.z, c3.w` 的源操作数 `r1` **缺省 swizzle 取 x 通道（红通道）**，r3.z≈0 → 光照丢失 z 主分量。
- 判别：PIL 统计法线贴图蓝通道 B≈254 → 资产健康，问题必在 shader。
- 修复：显式 swizzle `r1_bx2.xy` / `r1_bx2.z`；c3=(k,0,1,0)。

### 资产缺口补齐（DeepBump + TGA 松散部署）
- 流程：asset_annotated.csv 查主力贴图 → work_tmp\basegame_extract 提取源贴图 → `T3\ai_normalgen.py single`（Anaconda python，系统 python 缺 numpy）→ TGA 部署到 Art\Textures。
- 已补：主战坦克 extnkmzl01/nvbtmstr、兵营主体 ntbarracksslab、机场跑道 nbrunway。
- 后缀剥离机制：只补基名一张，_d/_e/_s 变体自动命中。

### 多材质 mesh 只探测 pass-0 贴图
- NBBARRACKS.MAIN BUILDING 有 8 张贴图，但 Detect_And_Build_Normal_Map 只看 pass-0 第一张（ntbarracksslab）——它缺资产则整 mesh 不升级 TG3、所有 pass 不挂。
- [NM_MISS] 日志直接给出探测贴图名，是定位此类问题的第一入口。

### NM_RENDER 日志致帧率雪崩（240→50）
- 根因：每栋建筑每次渲染调用 fprintf + 每 32 行 fflush，每帧上万行同步 stderr IO 阻塞渲染线程。
- 修复：日志完全移除；教训——渲染循环日志必须预算化（一次性/去重/采样），排查完即删。

## 六、踩坑清单（教训）

1. **"各面平等黑" = 光方向符号反**：dot(N,L) 的 L 必须指向光源；引擎 `Get_Light_Direction` 返回"指向光源"方向，手动构造光方向时极易写反（z 负 = 指向地下 → 屋顶全黑）。
2. **ps.1.4 没有 `nrm` 指令**（那是 ps.1.1 的）。用了会 `PS assemble FAILED err=(6)` → PS 创建失败 → dot3 静默不挂载 → 用户看到"正常游戏"其实是假象。**教训：每次部署必须 grep 日志确认 PS/VS create hr=0**。
3. **贴图切换检测的假阳性**：按 mesh 名记录"上次贴图"会把"同一模型多个材质 category 的帧内交替"（墙面→玻璃→墙面→玻璃）误判为"贴图在切换"。必须加 tick/帧号区分帧内双材质与帧间切换。
4. **同名 mesh 每帧渲染 2 遍是正常现象**（多个 render task/category），不能据此判断 bug；但两遍输出不同（一遍 dot3 一遍固定管线）时会闪。
5. **法线贴图有 TGA 和 DDS 双格式**：移走 TGA 后引擎仍能从 `_nm.dds` 加载（日志 companion resolved 依旧成功）。判别实验必须双格式一起移。
6. **用户观察力是宝贵信号**："像雪覆盖、部分覆盖"最终指向 lakedusk（黄昏天空灰白贴图）——不是雪地变体、不是 shroud，而是玻璃 submesh 的贴图本身。早期若顺着"雪"去查贴图名（lakedusk = 黄昏湖天空），能更快锁定。
7. **排除法顺序**：先做"整体开关"对照（dot3 全关），再逐层二分（光照→采样→资产），比从微观机制猜更高效。
8. **[NM_RENDER] 全量日志设计**：一条日志包含 tex/mirror/pass/applied/fvf/blend/tick 七维信息，一次运行即可回答"挂载稳定性、pass 构成、双遍渲染、混合模式"四个问题，比多轮打日志高效得多。

## 七、相关代码位置

- `Core\Libraries\Source\WWVegas\WW3D2\dx8renderer.cpp`（hook：挂载条件、[NM_RENDER] 诊断日志）
- `Core\Libraries\Source\WWVegas\WW3D2\dx8normalmap.cpp`（Apply/Restore、companion 查找）
- `Core\GameEngineDevice\Source\W3DDevice\GameClient\W3DShaderManager.cpp`（VS/PS 汇编）
- `work_tmp\run_nm.ps1`（GENERALSX_NORMALMAP / NOSTAGE1 / RAW 判别开关）
- T2 清单：`T2 模型清单解析器\model_manifest_by_unit.csv`（CB 行贴图列为空，需用运行时日志反查）
