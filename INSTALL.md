# MGPU Prism FG 2.0 中文安装与排障指南

## 一、安装前

这是实验性 ReShade add-on，不是独立启动器，也不替代游戏自己的 DLSS 超分。2.0 的实机重点是 D3D12 双卡，不代表所有 D3D11／D3D12 游戏都已兼容。

1. 备份游戏目录内已有注入与配置。
2. 不要混装另一条神经处理路径，不要同时开启游戏自身帧生成。
3. 优先测试离线单机。在线游戏和反作弊系统可能禁止注入、hook 或第三方覆盖层。
4. 安装支持 add-on 的 64 位 ReShade，并选择游戏实际使用的图形 API。
5. 合法自备 `nvngx_dlssnr.dll`；需要 DLSS FG 时还需 `nvngx_dlssg.dll`。本发布不提供任何 NVIDIA DLL 或模型。

## 二、复制文件

找到游戏**实际 exe**所在目录，将运行包内容复制进去，再自行放入 NVIDIA 模块：

```text
<游戏目录>/
├─ nvngx.dll_mgpu_bridge.addon64
├─ mgpu.ini
├─ gpu1.ini
├─ ReShade2.ini
├─ panel_simple.txt
├─ mgpu/
│  ├─ nvngx_dlssnr.dll        用户自备
│  └─ nvngx_dlssg.dll         仅 DLSS FG 需要，用户自备
└─ reshade-shaders/
   └─ Shaders/
      └─ mgpu_depth_tap.fx
```

注意：

- `nvngx.dll_mgpu_bridge.addon64` 必须保留完整文件名。
- 不要把 `nvngx_dlssnr.dll` 或 `nvngx_dlssg.dll` 放在 exe 旁。游戏或 Streamline 可能提前加载它们，导致 NGX 绑定到错误的显卡。
- 运行包不包含 `dxgi.dll` 或 `d3d12.dll`；它们应由 ReShade 安装器生成，不要覆盖游戏自身文件。
- 游戏端 `ReShade.ini` 的 `EffectSearchPaths` 与 `TextureSearchPaths` 必须能找到 `mgpu_depth_tap.fx`。
- 随包 `ReShade2.ini` 是副卡运行时配置，保持 `PresetPath=.\gpu1.ini`；它不能替代游戏端的深度 tap 配置。
- `gpu1.ini` 是副卡运行时的独立预设，避免重复执行游戏端效果。

## 三、默认配置与启用 4x

发布默认采用较保守的起点：

```ini
Neural=1
Passes=1
FrameGen=0
FGMulti=2
; FrameCap 未启用，即不添加插件帧率上限
```

在独立 Prism 面板中可调整遍数、每遍网络与分辨率、强度、风格以及 DLSS FG 2x／4x。**修改 FrameGen 或 FGMulti 后必须完整退出并重启游戏。** 面板写入配置不代表运行中的 NGX／DLSSG 代理已经重建。

若手工编辑 `mgpu.ini`：

```ini
Passes=3
FrameGen=4
FGMulti=4
```

这是启用三遍与 4x 的功能配置，不是对任意硬件的性能承诺。首次排障应先使用一遍、FG 关闭，确认基础链路后再逐项开启。

## 四、启动与面板

1. 完整退出游戏及其他大型 GPU 负载。
2. 启动游戏，等待神经画面出现；首次建立资源和编译着色器可能较慢。
3. 按小键盘 `*` 打开独立 Prism 面板；方向键调整，Tab 翻页，Enter 进入自定义调色台。
4. 先确认两张显卡分配、输出尺寸、神经更新和 DLSS FG 状态，再继续修改画质参数。
5. 如果键盘没有小键盘，在 `mgpu_panel.ini` 中设置：

```ini
[Panel]
OverlayKey=F8
```

修改热键后完整重启游戏。

## 五、画面与性能设置

- **遍数**：每增加一遍都会增加真实神经求值成本；三遍与一遍 FPS 相同不应被当成正常现象忽略。
- **网络模型**：Light-FP8 通常更快，Full-FP8 更耗费副卡；可用模型取决于用户自备的 DLSS NR 版本。
- **每遍网络分辨率**：降低比例可减少求值成本。当前向上调节存在已知失效问题，不要把滑块数字单独当成实际工作尺寸。
- **强度与风格**：更强不代表更稳定。出现色偏、黑雾或过度编辑时先恢复默认，不要同时调整多个参数。
- **角色守护**：用于角色与肤色区域的保护，不等于 NVIDIA 自动人脸遮罩，也不是通用抗拖影开关。
- **DLSS FG 2x／4x**：4x 表示每组三张生成帧加一张真实帧的目标路径；显示刷新率、输入缺失和回退都会影响最终上屏。
- **FrameCap**：只限制游戏侧 Present，不会提高神经求值速度。发布默认不启用。

## 六、常见问题

### 没有神经画面

依次检查：

1. ReShade 是否支持 add-on，日志是否注册 `MGPU Bridge`；
2. 插件完整文件名是否正确；
3. NVIDIA 模块是否只在 `mgpu` 子目录；
4. 深度 tap 是否存在且搜索路径正确；
5. 游戏是否提供了可识别的 DLSS／NGX 输入；
6. `ReShade.log` 中的 NGX、资源创建和 Arm 错误。

### DLSS FG 没有启动

1. 确认 `mgpu/nvngx_dlssg.dll` 存在且与当前驱动、显卡兼容。
2. 修改开关或倍率后必须完整重启。
3. 关闭游戏自身 FG 和其他神经插件。
4. 查看 `[MGPU][FG]` 的 `requested`、`cap`、`this-group`、`generated`、`presented-gen`、`invalid-input` 与 `errors`。
5. `generated` 只证明生成调用产出，`presented-gen` 只证明 Present 提交；真实显示对照还需 PresentMon 的显示事件或等效测量。

### 关闭神经画面后卡住

2.0 已加入 FG pacing 与主 worker 的交接修复，并通过自动测试，但仍待完整实机复验。遇到问题时先正常退出游戏，保留日志，不要继续反复切换开关。

### 运动闪烁、拖影或 FG 伪影

1. 先关闭 FG 并重启，区分神经链与插帧问题。
2. 回到一遍，判断问题是否随额外神经遍数出现。
3. 恢复默认调色与角色守护，排除过强编辑。
4. 不要在同一轮同时修改运动向量、分辨率、强度和帧率上限。
5. 当前版本仍可能有轻微快速运动闪烁；不能承诺零拖影或零伪影。

### 分辨率或画面比例异常

正常退出游戏，恢复游戏原生显示设置后重启。不要在 Armed 状态修改游戏分辨率、DLSS 模式或会重建交换链的选项。

## 七、卸载

完整退出游戏，移走 `nvngx.dll_mgpu_bridge.addon64` 即可停用本插件。按需要移除随包配置和 `mgpu_depth_tap.fx`；不要删除游戏自身文件。用户自备的 NVIDIA 模块与 ReShade 是否保留，由其他程序的需求决定。

## 八、反馈时附带

- 游戏与版本、exe 名称、D3D11／D3D12；
- 两张显卡型号、驱动版本与显示器连接方式；
- 输出分辨率、遍数、每遍网络尺寸与 FG 倍率；
- 游戏生产、神经更新、Present 与真实显示帧率，分别记录；
- 问题发生前是否切换过神经开关、分辨率或 DLSS；
- 脱敏后的 `ReShade.log` 关键片段与复现步骤。
