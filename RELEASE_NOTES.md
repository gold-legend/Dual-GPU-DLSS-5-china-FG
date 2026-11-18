# MGPU Prism FG 2.1

双卡 DLSS 神经渲染、三遍串行 Prism 处理，以及位于神经输出之后的 DLSS Frame Generation 2x／4x。

本版基于 MGPU／Neural Coprocessor 0.6.0，在 2.0 的三遍串行、每遍自定义控制和副卡 DLSS FG 基础上，删除当前路径中的无效历史捕获，复用肤色遮罩计算，并补充分段性能计时与面板显示修正。

> **实验性预发布版本。** 不承诺所有游戏兼容、精确四倍、零闪烁、零拖影或固定性能提升。优先用于离线单机。

## 2.1 修改思路

1. **先分段计时，再优化**：把每遍拆成 input＋Down、NGX、Fit＋Compose 三段，在副卡上记录只读 GPU 时间，避免把游戏生产、跨卡传输、神经求值和帧生成混成一个 FPS 数字。
2. **删除无消费者的 Capture**：当前 PX1 路径关闭时域残余历史，着色器也没有实际读取 `gHistory`，因此移除每遍无效的 `CS_Capture` 调度、描述符写入和资源状态转换；保留旧路径资源与分支，不改神经算法。
3. **复用肤色遮罩**：每个像素只计算一次 `SkinMask`，同一数值供两次 GuardEdit 和肤色分支使用，颜色数学不变。
4. **保持功能边界**：三遍串行、第三遍最终输出、每遍分辨率／强度／风格、Prism 调色、角色守护和 DLSS FG 2x／4x 均保留。
5. **修正面板读数**：网络分辨率控件读取第一遍实际比例，避免全部设为 30% 时仍显示旧全局值。
6. **不为任何显卡预设帧率上限**：发布默认仍为 `Passes=1`、`FrameGen=0`、`FGMulti=2`，不启用插件 FrameCap。

## 性能实测

《剑星》开发机：RTX 3070 Ti 渲染、RTX 5060 Ti 神经处理、3840×2160，三遍串行并开启 4x。以下为副卡 GPU 分段平均耗时：

| 三遍区段 | 优化前 600 帧 | 优化后累计 1200 帧 |
|---|---:|---:|
| P1：input＋Down／NGX／Fit＋Compose | 0.089／2.639／0.794 ms | 0.094／2.604／0.776 ms |
| P2：input＋Down／NGX／Fit＋Compose | 0.047／2.759／0.976 ms | 0.046／2.674／0.907 ms |
| P3：input＋Down／NGX／Fit＋Compose | 0.059／7.075／2.222 ms | 0.060／6.988／2.124 ms |
| 三遍 Fit＋Compose 合计 | 3.992 ms | 3.807 ms |
| 三遍主体合计 | 16.660 ms | 16.273 ms |

合成段合计减少约 **0.185 ms（4.6%）**，三遍主体合计减少约 **0.387 ms**。两轮样本量和场景不完全一致，这不是严格同帧 A/B，也不能直接换算为固定 FPS 提升。4x 与输出路径另有开销，不包含在上述三遍 NGX 数字中。

## 构建与运行验证

- 2.1 Release 构建成功，版本字符串为 `2.1.0-prism`。
- CTest 10／10 通过；神经开关交接与 PX1 抗闪结构的 Python 检查通过，其中三个相关 HLSL 着色器编译成功。
- 自动测试不等于真实双卡、NGX、DLSSG、画质、上屏或延迟验收。
- 优化版已在三遍＋4x 实机路径运行，未见着色器编译、PSO 创建或 NGX EvaluateFeature 错误。
- **优化后的实机画质仍待用户复验**，本版不宣称闪烁或拖影已经修复。

## 2.0 基线能力的当前证据

2.0 的 4x 日志持续记录 `requested=4x`、`this-group=4x`、生成与呈现计数，且 `errors=0`；`DXGI_FRAME_STATISTICS` 也报告了显示帧与 vblank 间隔。这支持 4x 路径正在运行并有帧进入显示统计。

**PresentMon 三次采样均因大量 ETW 事件丢失而没有生成可用 CSV，因此独立外部真实显示帧率对照尚未完成。** `generated` 与 `presented-gen` 不等于已经独立验证实际上屏，更不等于任何场景精确四倍。

## 已知问题

- 静止画面较稳定；轻微、快速的运动闪烁仍可能存在，拖影也不能宣称普遍消除。
- 每遍自定义分辨率向上调节仍存在失效问题。
- 关闭神经画面的交接修复通过自动测试，尚待用户实机复验。
- DLSS FG 仍可能出现果冻、细小物体或遮挡伪影。
- 缺少多机型、多游戏、长时间、帧间隔与端到端输入延迟验收。

## 安装前必读

1. 备份现有注入与配置，不要并行第二条神经路径或游戏自身 FG。
2. 安装支持 add-on 的 64 位 ReShade。
3. 插件文件名保留为 `nvngx.dll_mgpu_bridge.addon64`。
4. 用户合法自备的 `nvngx_dlssnr.dll` 放入 `mgpu` 子目录；使用 DLSS FG 还需自备 `mgpu/nvngx_dlssg.dll`。
5. 确保游戏端 ReShade 搜索路径能找到 `mgpu_depth_tap.fx`。
6. 在面板修改 FrameGen 或倍率后，必须完整退出并重启游戏。
7. 运行中不要修改游戏分辨率、DLSS 模式或会重建交换链的选项。

完整步骤见仓库 `INSTALL.md`。

## 下载

- `MGPU-Prism-FG-2.1-Runtime.zip`：2.1 插件、默认配置、深度 tap、中文安装说明与许可。
- `MGPU-Prism-FG-2.1-Source.zip`：源码、构建脚本、测试与文档。
- `SHA256SUMS.txt`：附件完整性校验。

**不包含** `_nvngx.dll`、`nvngx_dlssnr.dll`、`nvngx_dlssg.dll`、其他 NVIDIA DLL、模型、ReShade 安装器、游戏文件或私人日志。NVIDIA SDK 构建头文件由构建者阅读官方许可后通过随附脚本获取。

## 致谢

感谢 Marcelo Guibout 的 MGPU／Neural Coprocessor、ChefZC／ChefDavid0815 的 Prism NR，以及 ReShade、Dear ImGui、MinHook、DLSSNR-Cost-Scaler 和 JetBrains Mono 的作者与贡献者。各组件保留原许可；本分支不是上述作者或厂商的官方版本。
