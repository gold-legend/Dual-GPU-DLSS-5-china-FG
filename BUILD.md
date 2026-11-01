# 构建说明

## 已核验的工具链

本次 1.0 基线在 Windows 上使用以下方式构建成功：

- CMake 3.21 或更高；
- Visual Studio 2022，x64 C++ 工具链与 Windows SDK；
- C++20；
- 配置阶段可访问 Dear ImGui 官方固定版本头文件。

发布包中的 AMD、ReShade、MinHook 头文件以及已生成的 AMD 光流排列头文件已按源码包布局整理。NVIDIA 与 Intel 的构建头文件仍需构建者按各自许可准备。

## 一、获取 NVIDIA 头文件

先阅读 [NVIDIA RTX SDK 许可](https://github.com/NVIDIA/DLSS/blob/a291cc7d2cc642a51566f3dfd5376f635cd1b284/LICENSE.txt)，确认自己的使用与分发方式符合条款。然后在 PowerShell 中运行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\tools\fetch_ngx_headers.ps1
```

脚本从 `NVIDIA/DLSS` 的固定提交 `a291cc7d2cc642a51566f3dfd5376f635cd1b284` 下载公开头文件到 `ext/ngx`，不会下载 NVIDIA DLL 或模型，也不会替你发布任何内容。

NVIDIA 许可不允许把 SDK 当作独立产品随意再分发，因此源码包没有直接塞入这套头文件。保留版权与许可约束，不代表把 NVIDIA 组件改成 MIT。

## 二、准备 Intel 头文件

从自己的 Intel XeSS FG SDK 复制原样头文件，形成：

```text
third_party/intel_xess/xess_fg/xefg_swapchain.h
third_party/intel_xess/xess_fg/xefg_swapchain_d3d12.h
third_party/intel_xess/xell/xell.h
third_party/intel_xess/xell/xell_d3d12.h
```

源码包只附 Intel 许可和放置说明，不直接再分发这些源码头文件。运行包附原样 Intel DLL 与许可，两者不要混为一谈。

## 三、配置与编译

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

CMake 配置会从 Dear ImGui 固定提交 `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c` 获取 `imgui.h` 和 `imconfig.h`，版本为 1.92.5。不要混用其他 ImGui ABI。若下载失败，原构建逻辑会关闭 ReShade ImGui 面板并继续编译；独立面板仍在，但应将这类构建与完整构建区分记录。

## 四、源码包相对布局

```text
MGPU-Prism-FG-1.0/
├─ CMakeLists.txt
├─ src/
├─ generated/ffx_opticalflow/
├─ ext/
│  ├─ reshade/                  已附公开 add-on 头文件
│  └─ ngx/                      运行脚本后生成
└─ third_party/
   ├─ amd_fidelityfx/            仅所需、许可白名单内的文件
   ├─ intel_xess/
   └─ minhook/
```

为便于仓库直接构建，发布副本调整了 CMake 的 AMD 依赖路径。原开发布局通过 Magpie include 目录提供的只有一个 `PIX_COLOR` 编译宏；发布副本在 AMD 后端目录用等价的最小兼容定义替代，不包含 Microsoft PIX 头文件、PIX 运行库或 Magpie 源码。项目原有 `.cpp`、`.hpp`、着色器及生成头文件没有为打包改写。发布运行包仍使用事先核验的稳定插件，而不是用路径整理后的临时产物替代。

`ext/reshade` 的公开头文件来自 ReShade 固定提交 `18deaa52de0c425a78b329e9cb3c497281cd00ec`，add-on API 20。本项目选择其 BSD 三条款许可。

## 五、可选测试

```powershell
cmake -S . -B build-tests -G "Visual Studio 17 2022" -A x64 -DMGPU_BUILD_TESTS=ON
cmake --build build-tests --config Release --parallel 4
ctest --test-dir build-tests -C Release --output-on-failure
```

这些主要是 CPU 侧策略与诊断状态机测试，**不等于跨显卡、NGX、光流、XeSS、画质或延迟的实机验收**。

## 六、AMD 光流生成文件

`generated/ffx_opticalflow` 已附，因此正常编译不需要再次运行 DXC。只有修改光流 HLSL 或生成流程时，才需要查看 `tools/generate_optical_flow.py` 的参数并准备兼容的 DXC。

该脚本从 AMD HLSL 生成多种着色器排列。不要混用另一版 SDK 的 C++ 与已生成头文件；资源描述、绑定和着色器接口必须保持一致。

## 七、复现边界

- 发布布局已经独立完整构建成功；重建产物与稳定插件大小一致，10 字节差异已核验为 4 字节 PE／调试目录时间戳与 6 字节内嵌编译时间字符串。
- 运行包使用事先核验的稳定插件，而不是以本次重建产物替代；上述结果不代表其他机器或工具链必然得到相同哈希。
- 没有把历史 CI 配置带入发布包，因为旧配置没有覆盖完整的 AMD 与其他本地依赖，不能宣称一条 CI 即可复现。
- 源码包含研发诊断与未开放旧路径。构建成功、开关存在和实机验收通过是三个不同结论。
