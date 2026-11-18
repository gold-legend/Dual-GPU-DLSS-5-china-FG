# MGPU Prism FG 2.0

双卡 DLSS 神经渲染、三遍串行 Prism 处理，以及位于神经输出之后的 DLSS Frame Generation 2x／4x。

本版基于 MGPU／Neural Coprocessor 0.6.0，保留跨卡颜色、深度与运动信息链，加入 Prism 独立中文面板、PX1 当前帧重建和每遍控制。2.0 主路径改用 NVIDIA DLSS FG，不再使用 1.0 的 AMD Optical Flow／XeSS FG。

> **实验性预发布版本。** 不承诺所有游戏兼容、精确四倍、零闪烁、零拖影或固定性能提升。优先用于离线单机。

## 主要更新

- 三遍串行神经求值，第三遍作为最终输出；各遍保留独立历史。
- 每遍网络、分辨率、强度和风格控制；Prism 调色与角色守护保留。
- 修复跨卡预取时颜色、深度、运动向量与帧序号的同帧绑定。
- PX1 运动残余约束：保留静止清晰度，减轻运动中的编辑残余闪烁。
- 副卡 DLSS FG 2x／4x：4x 生成三张不同时间位置的中间帧，不用重复帧填充冒充 4x。
- 神经开关与 FG pacing 的 Present 交接修复，避免两个线程同时操作共享呈现资源。
- 发布默认不启用 FrameCap，不为其他显卡或用户预设 30／55／60 FPS 上限。

## 当前运行证据与限制

《剑星》测试机：RTX 3070 Ti 渲染，RTX 5060 Ti 神经处理，3840×2160。

日志持续记录 `requested=4x`、`this-group=4x`、生成与呈现计数，且 `errors=0`；`DXGI_FRAME_STATISTICS` 也报告了显示帧与 vblank 间隔。这支持 4x 路径正在运行并有帧进入显示统计。

**PresentMon 三次采样均因大量 ETW 事件丢失而没有生成可用 CSV，因此独立外部真实显示帧率对照尚未完成。** `generated` 与 `presented-gen` 不等于已经独立验证实际上屏，更不等于任何场景精确四倍。

## 已知问题

- 静止画面较稳定；运动闪烁已明显减轻，但仍有轻微快速闪烁。
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
6. 发布默认 `Passes=1`、`FrameGen=0`、`FGMulti=2`，不启用插件帧率上限。
7. 在面板修改 FrameGen 或倍率后，必须完整退出并重启游戏。
8. 运行中不要修改游戏分辨率、DLSS 模式或会重建交换链的选项。

完整步骤见仓库 `INSTALL.md`。

## 下载

- `MGPU-Prism-FG-2.0-运行包.zip`：2.0 插件、默认配置、深度 tap、中文安装说明与许可。
- `MGPU-Prism-FG-2.0-源码与文档.zip`：源码、构建脚本、测试与文档。
- `SHA256SUMS.txt`：附件完整性校验。

**不包含** `_nvngx.dll`、`nvngx_dlssnr.dll`、`nvngx_dlssg.dll`、其他 NVIDIA DLL、模型、ReShade 安装器、游戏文件或私人日志。NVIDIA SDK 构建头文件由构建者阅读官方许可后通过随附脚本获取。

## 构建验证

2.0 Release 构建成功，CTest 10／10 通过。自动测试不等于真实双卡、NGX、DLSSG、画质、上屏或延迟验收。

## 致谢

感谢 Marcelo Guibout 的 MGPU／Neural Coprocessor、ChefZC／ChefDavid0815 的 Prism NR，以及 ReShade、Dear ImGui、MinHook、DLSSNR-Cost-Scaler 和 JetBrains Mono 的作者与贡献者。各组件保留原许可；本分支不是上述作者或厂商的官方版本。
