# MGPU Prism FG 2.1

**双卡 DLSS 神经渲染、三遍串行 Prism 处理，以及位于神经输出之后的 DLSS Frame Generation 2x／4x。**

这是基于 **MGPU／Neural Coprocessor 0.6.0** 与 **Prism NR** 的社区二次开发版本。主卡渲染游戏，副卡执行神经处理与帧生成；当前主路径使用 NVIDIA DLSS FG，不再沿用 1.0 的 AMD Optical Flow／XeSS FG。

> **2.1 是实验性预发布版本。** 它不是 NVIDIA、Prism NR 或游戏厂商的官方组件，不承诺所有游戏兼容、精确四倍、零闪烁或零拖影。请先在离线单机中测试。

## 2.1 的修改思路

- **先计时定位**：为三遍分别增加 input＋Down、NGX、Fit＋Compose 的只读副卡 GPU 分段计时，避免用单一 FPS 误判瓶颈。
- **删除无效历史捕获**：当前 PX1 路径没有实际读取 `gHistory`，因此不再每遍执行无消费者的 `CS_Capture`；旧路径代码和资源保留。
- **复用肤色遮罩**：`SkinMask` 每像素计算一次并复用于 GuardEdit 与肤色分支，不改变颜色数学。
- **修正面板读数**：网络分辨率控件读取第一遍实际比例，处理全部 30% 时面板仍显示旧全局值的问题。
- **保留用户功能**：三遍串行、第三遍最终输出、每遍自定义控制、Prism 调色、角色守护和 DLSS FG 2x／4x 不变。

本机分段记录中，三遍 Fit＋Compose 合计由 3.992 ms 降至 3.807 ms，减少约 0.185 ms；两轮不是严格同场景同帧 A/B，不能据此承诺固定 FPS 提升。优化后的实机画质仍待用户复验。详细数据见[实测与验收边界](RESULTS.md)。

## 保留的 2.0 基线能力

- **三遍串行神经处理**：三遍使用各自独立的 NGX 历史，最终颜色取第三遍输出，不把第一遍或第二遍误当成最终画面。
- **Prism PX1 当前帧重建**：在低分辨率神经结果上拟合编辑量，再合成到当前原生颜色；不是简单拉伸低分辨率神经图。
- **每遍独立控制**：独立面板提供遍数、网络模型、网络分辨率、强度和风格控制。自定义分辨率上调仍存在已知失效问题，详见下文。
- **DLSS FG 2x／4x**：在副卡上、神经处理之后生成并呈现输出帧。2x 每组生成一张中间帧，4x 每组生成三张不同时间位置的中间帧。
- **跨卡同帧绑定**：颜色、深度、运动向量与游戏帧序号绑定，减少跨卡预取时把不同帧输入拼在一起的风险。
- **PX1 运动残余约束**：只约束运动中的编辑残余，尽量保留静止清晰度；本机观察为运动闪烁明显减轻，但仍有轻微、频率较快的闪烁。
- **神经开关交接**：关闭神经画面时先结束 FG pacing 线程，再交还主 worker 呈现；该修复已通过自动测试，尚待完整实机复验。
- **不设发布默认帧率上限**：`FrameCap` 默认关闭。测试机曾使用 30 FPS 上限，那不是给其他用户设置的推荐值。

处理链与计数器含义见[架构说明](ARCHITECTURE.md)。

## 当前实测能说明什么

测试环境为《剑星》、Windows 11、RTX 3070 Ti 渲染、RTX 5060 Ti 神经处理，3840×2160 输出。

2.0 基线的运行日志持续记录 `requested=4x`、`this-group=4x`、生成帧和真实帧呈现，且 `errors=0`；桥接器的 `DXGI_FRAME_STATISTICS` 也报告了显示帧与 vblank 间隔。这支持 **4x 路径正在生成、提交并有帧进入显示统计**。

但 PresentMon 2.6.0 三次采样都出现大量 ETW 事件丢失，未生成可用 CSV，因此 **尚未完成独立 PresentMon 的开启／关闭真实显示帧率对照**。不能把 `generated`、`presented-gen` 或面板总 FPS 单独写成“外部工具确认精确四倍”。

详细条件与未验收项目见[实测与验收边界](RESULTS.md)。

## 运行环境

- 64 位 Windows；本机记录来自 Windows 11。
- 两张可用显卡；游戏渲染卡与神经处理卡由实际显示与 Windows 图形设置决定。
- 神经处理卡必须能运行用户本机的 DLSS NR 与 DLSS FG。
- 游戏需能安装支持 add-on 的 64 位 ReShade；2.1 的实机重点是 D3D12。
- 用户需合法自备 `nvngx_dlssnr.dll`；使用 DLSS FG 时还需自备 `nvngx_dlssg.dll`。**项目不分发 NVIDIA DLL 或模型。**

## 安装

按照[中文安装与排障指南](INSTALL.md)操作。核心目录如下，`<游戏目录>` 指游戏实际 exe 所在目录：

```text
<游戏目录>/
├─ nvngx.dll_mgpu_bridge.addon64
├─ mgpu.ini
├─ gpu1.ini
├─ ReShade2.ini
├─ mgpu/
│  ├─ nvngx_dlssnr.dll        用户自备，不随包提供
│  └─ nvngx_dlssg.dll         仅 DLSS FG 需要，用户自备
└─ reshade-shaders/
   └─ Shaders/
      └─ mgpu_depth_tap.fx
```

插件文件名中的 `nvngx.dll` 是完整文件名的一部分，不要删掉。NVIDIA 模块只放在 `mgpu` 子目录，不要放在游戏 exe 旁。

发布配置默认 `FrameGen=0`、`FGMulti=2`、`Passes=1`，且不设置插件帧率上限。面板中选择 DLSS FG 2x／4x 后，**完整退出并重启游戏**才会创建或关闭对应生成路径。

## 面板与快捷键

| 操作 | 按键 |
|---|---|
| 打开／关闭独立 Prism 面板 | 小键盘 `*` |
| 选择与调整 | 方向键 |
| 翻页 | `Tab` |
| 进入自定义调色台 | `Enter` |
| 手动 Arm | `Ctrl＋Alt＋F10` |
| 仅在 Dcomp 模式隐藏／恢复神经画面 | `Ctrl＋Alt＋F6` |

`Home` 属于 ReShade。独立面板配置位于 `mgpu_panel.ini`；可把 `[Panel]` 下的 `OverlayKey=Multiply` 改为 `OverlayKey=F8` 等受支持键名。

## 已知问题与使用边界

- 静止画面较稳定，但运动时仍可能出现轻微快速闪烁；DLSS FG 还可能产生果冻、遮挡或细小物体伪影。
- **每遍自定义分辨率向上调节仍有失效问题**，不能宣称任意自定义比例都已生效；降低分辨率与上调后的复验要分开记录。
- 关闭神经画面的线程交接修复已做自动测试，但尚未完成用户实机确认。
- 2.1 的合成开销优化不构成闪烁／拖影修复，优化后的实机画质仍待复验。
- 帧生成只增加输出帧，不同步提高游戏模拟率、输入采样率或神经求值率。
- 不要同时启用游戏自身 FG 或第二条神经处理路径。
- 运行中不要修改游戏分辨率、DLSS 模式或会重建交换链的选项；先退出再修改。
- 注入、窗口 hook 与第三方覆盖层可能触发反作弊；在线游戏的账号风险由使用者承担。
- 尚无多机型、多游戏、长时间稳定性与端到端输入延迟验收。

## 源码与构建

核心实现：

```text
src/gpu1_context.cpp       多遍神经链、最终输出、跨卡输入绑定与分段计时
src/prism_x1_multi.cpp     独立 PX1 实例与三遍包装
src/prism_x1.cpp           GuidedFit、当前帧重建、GuardEdit 与合成
src/prism_finish.cpp       Prism 调色、风格与角色守护
src/fg_gpu1.cpp            副卡 DLSS FG 2x／4x、调度和 Present 交接
src/poverlay.cpp           独立中文面板
src/worker.cpp             神经开关与呈现线程交接
```

2.1 Release 构建成功，CTest 10／10 通过。构建工具、依赖获取和复现边界见[构建说明](BUILD.md)。源码包不附带 NVIDIA SDK 头文件；构建者需阅读官方许可后运行 `tools/fetch_ngx_headers.ps1` 获取固定版本。

## 致谢与授权

- MGPU／Neural Coprocessor：Marcelo Guibout，MIT。
- Prism NR：ChefZC／ChefDavid0815，MIT。
- ReShade：Crosire 与贡献者，BSD 三条款。
- Dear ImGui、MinHook、DLSSNR-Cost-Scaler、JetBrains Mono：各自原许可。

详细归属见[第三方授权说明](THIRD_PARTY.md)。项目主体与新增代码沿用 [MIT 许可](LICENSE)，不代表上述作者或厂商为本分支背书。

## 游戏本地参数对照

这些文件是本机实机配置快照，用于玩家对照，不代表所有机器都应照抄。先退出游戏，再把目标文件复制为游戏 exe 旁的 `mgpu.ini`；替换前保留自己的配置。

- [剑星本地参数](configs/stellar-blade.ini)：RTX 3070 Ti 渲染＋RTX 5060 Ti 神经处理，3840×2160、D3D12、三遍网络分辨率 69%／58%／58%、前两遍 `NRPreset=0`、第三遍 `NRPreset3=1`、`NRVectors=chain`、`PrismNRVecZero=0` 和 FG 4x。NR 保留真实游戏向量；不要混入异环的 `MVecSource=3`。文件故意不写 `FGPairFlow`，用于对照 2.4 的全局缺省开启规则。`FrameCap=115`、`Perf11=1`、`FGInstr=1`、`LatSplit=1` 是本机测试值，不是通用推荐；不需要 115 FPS 上限时删除 `FrameCap` 行。
- [异环本地参数](configs/yihuan.ini)：保留该游戏的 NR 零向量路径，与剑星的真实 NR 向量路线不同。FGPairFlow 是 FG 的独立运动来源，不改变 NR 的每游戏向量选择。

以上快照只说明对应实机配置和运行验证条件，不承诺其他机器无闪烁、无拖影或相同帧率。

## 发布文件

- `MGPU-Prism-FG-2.1-Runtime.zip`：插件、默认配置、深度 tap、中文安装说明与许可。
- `MGPU-Prism-FG-2.1-Source.zip`：可审阅源码、构建脚本、测试与文档。
- `SHA256SUMS.txt`：附件完整性校验。
