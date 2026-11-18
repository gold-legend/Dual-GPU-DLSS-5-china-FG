# MGPU Prism FG 2.0 第三方授权说明

本项目是多个许可组件的组合。项目主体与新增代码沿用 MIT，不表示第三方 SDK、字体、运行库或商标都被重新授权为 MIT。保留原有版权、许可和免责声明，不代表任何原作者或厂商为本分支背书。

## 项目主体

- **MGPU／Neural Coprocessor**：Copyright（c）2026 Marcelo Guibout，MIT。
- **本分支新增与修改**：沿用根目录 [LICENSE](LICENSE)。
- 基础项目来源：[Neural-coprocessor](https://github.com/maohgad-web/Neural-coprocessor)。

## Prism NR

Copyright（c）2026 ChefZC，MIT。

本项目使用 Prism NR 的独立面板、调色、PX1 编辑量拟合与角色守护等思路，并在 MGPU 的多遍副卡链上重新适配。它不是 Prism NR 官方双卡版本。

- 来源：[prism-nr](https://github.com/ChefDavid0815/prism-nr)。
- 许可原文：[licenses/Prism-NR-MIT.txt](licenses/Prism-NR-MIT.txt)。
- 相关代码：`src/prism_x1.cpp`、`src/prism_x1_multi.cpp`、`src/prism_finish.cpp`、`src/prism_grade.h`、`src/prism_look_hlsl.h`、`src/poverlay.cpp`。

## ReShade

公开 add-on 头文件来自 `crosire/reshade` 的固定提交 `18deaa52de0c425a78b329e9cb3c497281cd00ec`。头文件采用 BSD 三条款／MIT 双许可，本项目选择 **BSD 三条款**。

- 许可原文：[licenses/ReShade-BSD-3-Clause.txt](licenses/ReShade-BSD-3-Clause.txt)。
- 本包附头文件，但不附 ReShade 安装器或运行时；用户需自行安装支持 add-on 的版本。

## Dear ImGui

Copyright（c）2014—2025 Omar Cornut，MIT。

CMake 配置阶段按固定提交获取 `imgui.h` 与 `imconfig.h`，版本 1.92.5。项目不编译独立 ImGui 库；相应声明及内联代码按其许可使用。

- 许可原文：[licenses/Dear-ImGui-MIT.txt](licenses/Dear-ImGui-MIT.txt)。

## DLSSNR-Cost-Scaler

Copyright（c）2026 xen，MIT。

`src/Downsample_Shader.h`、`src/Resolve_Shader.h` 来自 `xenmods/DLSSNR-Cost-Scaler`；`src/residual.cpp` 也包含基于其着色器的适配。

- 许可原文：[licenses/DLSSNR-Cost-Scaler-MIT.txt](licenses/DLSSNR-Cost-Scaler-MIT.txt)。

## MinHook

Copyright（C）2009—2017 Tsuda Kageyu；Hacker Disassembler Engine 部分 Copyright（c）2008—2009 Vyacheslav Patkov。

- 许可与附加版权原文：[licenses/MinHook.txt](licenses/MinHook.txt)。
- 源码头文件与实现位于 `third_party/minhook`。

## NVIDIA DLSS／NGX

- 源码包与运行包均**不包含** `_nvngx.dll`、`nvngx_dlssnr.dll`、`nvngx_dlssg.dll`、DLSS 模型或其他 NVIDIA 二进制。
- 源码包不直接再分发 NVIDIA 源码头文件。`tools/fetch_ngx_headers.ps1` 从官方固定提交获取构建所需公开头文件，使用者需先阅读并接受官方 SDK 许可。
- 用户自备的 NVIDIA 模块放在游戏目录的 `mgpu` 子目录，不随本项目分发。
- NVIDIA 的 SDK、商标、商业发布通知等义务不因本项目使用 MIT 而消失；发布者与使用者应单独审阅[官方许可](https://github.com/NVIDIA/DLSS/blob/a291cc7d2cc642a51566f3dfd5376f635cd1b284/LICENSE.txt)。

## JetBrains Mono

独立面板使用 JetBrains Mono 字体生成的内嵌数据，采用 SIL Open Font License 1.1。

- 版权与许可原文：[licenses/JetBrainsMono-OFL.txt](licenses/JetBrainsMono-OFL.txt)。
- 对应文件：`src/font_mono_regular.h`、`src/font_mono_bold.h`。

## 1.0 历史组件

1.0 曾使用 AMD FidelityFX Optical Flow、Intel XeSS FG／XeLL；2.0 主路径不再编译或分发这些运行组件。对应源码、二进制和说明只保留在 `v1.0.0` 历史标签中，不应把 1.0 的许可或实测描述当成 2.0 的当前依赖与结论。

## 游戏名称与运行记录

文档中的《剑星》仅用于说明本机运行状态，游戏画面和名称的权利属于原权利人。2.0 发布文档不附带新的游戏截图，也不把单次日志截面作为厂商认可的性能宣传。
