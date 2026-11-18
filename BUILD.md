# MGPU Prism FG 2.0 构建说明

## 已使用的工具链

本次 2.0 在 Windows 上使用以下工具构建成功：

- CMake 3.21 或更高；
- Visual Studio 2022，x64 C++ 工具链与 Windows SDK；
- C++17；
- 配置阶段可访问 Dear ImGui 官方固定版本头文件。

仓库包含 ReShade add-on 公开头文件、MinHook、所需第三方许可，以及本分支的 native-input ABI 头文件。NVIDIA SDK 头文件不直接再分发，需要按下一节获取。

## 一、获取 NVIDIA 头文件

先阅读 [NVIDIA RTX SDK 许可](https://github.com/NVIDIA/DLSS/blob/a291cc7d2cc642a51566f3dfd5376f635cd1b284/LICENSE.txt)，确认自己的使用方式符合条款，然后在 PowerShell 中运行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\tools\fetch_ngx_headers.ps1
```

脚本从 `NVIDIA/DLSS` 的固定提交 `a291cc7d2cc642a51566f3dfd5376f635cd1b284` 下载公开头文件到 `ext/ngx`，并逐个核对 SHA-256。它不会下载 NVIDIA DLL 或模型。

2.0 需要 DLSS NR 与 DLSS FG 的声明；脚本获取 `nvsdk_ngx.h`、`nvsdk_ngx_defs.h`、`nvsdk_ngx_params.h` 和 `nvsdk_ngx_defs_dlssg.h`。不要把用户运行时的 NVIDIA DLL 当成构建头文件来源。

## 二、配置与编译

在源码根目录执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target mgpu_bridge --parallel 4
```

产物为：

```text
build/mgpu_bridge.addon64
```

部署时复制并重命名：

```powershell
Copy-Item .\build\mgpu_bridge.addon64 .\nvngx.dll_mgpu_bridge.addon64
```

CMake 配置会从 Dear ImGui 固定提交 `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c` 获取 `imgui.h` 与 `imconfig.h`，版本为 1.92.5。不要混用其他 ImGui ABI。下载失败时，原构建逻辑会关闭 ReShade ImGui 面板并继续编译；独立 Prism 面板与这类构建必须区分记录。

## 三、源码布局

```text
MGPU-Prism-FG-2.0/
├─ CMakeLists.txt
├─ src/
├─ assets/
├─ tests/
├─ ext/
│  ├─ reshade/                         已附公开 add-on 头文件
│  └─ ngx/                             运行获取脚本后生成，不入库
└─ third_party/
   ├─ minhook/                         源码与原许可
   └─ mgpu_native_input/include/       本分支 native-input ABI 头文件
```

2.0 主路径不使用 1.0 的 AMD Optical Flow、XeSS FG 或 Intel XeLL。旧版完整内容保留在 `v1.0.0` 标签中。

## 四、自动测试

```powershell
cmake -S . -B build-tests -G "Visual Studio 17 2022" -A x64 -DMGPU_BUILD_TESTS=ON
cmake --build build-tests --config Release --parallel 4
ctest --test-dir build-tests -C Release --output-on-failure
python tests/neural_present_handover_test.py
python tests/prism_despeckle_test.py
```

本次 Release 构建的 CTest 结果为 **10／10 通过**。这些是 CPU 侧策略、节奏和诊断状态机测试；Python 脚本检查关键源代码交接／约束结构。它们不等于双卡、NGX、DLSSG、画质或延迟的实机验收。

## 五、复现边界

- 发布运行包使用本次 2.0 Release 构建产物，版本日志字符串为 `2.0.0-prism`。
- 打包路径调整了 native-input 头的相对位置；发布副本已在独立构建目录完成重建与 10 项 CTest 复验。
- 本次独立配置下载 ImGui 头文件超时，使用可选 ReShade ImGui 面板关闭的既定回退；独立 Prism 面板仍参与构建。
- 不同工具链和构建时间可能改变 PE 时间戳与内嵌构建时间，因此不承诺不同机器得到相同哈希。
- 源码保留研发诊断和旧路径。文件存在、开关存在、编译成功与实机验收通过是四个不同结论。
- 不包含 ReShade 运行时、NVIDIA SDK 头文件、NVIDIA DLL、模型、游戏文件、私人路径、日志或构建缓存。
