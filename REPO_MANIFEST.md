# 仓库内容与发布清单

## 文档

- `README.md`：仓库首页，项目定位、功能、实测、安装入口和限制。
- `ARCHITECTURE.md`：固定 1.5、PX1、Carry、AMD 光流、XeSS 与队列的机制。
- `RESULTS.md`：实测截面、构建一致性与未完成的验收。
- `INSTALL.md`：中文安装、配置、快捷键和排障。
- `BUILD.md`：真实依赖布局、构建命令和复现边界。
- `THIRD_PARTY.md`：第三方归属、许可路径和不分发项。
- `RELEASE_NOTES.md`：可粘贴到 GitHub Release 的中文说明。
- `MANUAL_PUBLISH.md`：验收后手动上传步骤。
- `CHANGELOG.md`：1.0 更新范围。

## 源码与资源

- `src`：1.0 基线的完整插件源码，包括保留的研发诊断和旧路径。
- `generated/ffx_opticalflow`：已生成的 AMD 光流排列头文件。
- `ext/reshade`：ReShade 公开 add-on 头文件及 BSD 三条款许可。
- `third_party/amd_fidelityfx`：构建所需且处于 AMD MIT 许可白名单中的文件。
- `third_party/minhook`：MinHook 源码与许可。
- `third_party/intel_xess`：许可与头文件放置说明；Intel 源码头文件由构建者自备。
- `tests`：可选 CPU 侧测试。
- `assets`：深度 tap、副卡配置等运行资源；运行包使用另写的中文发布配置与安装说明。
- `tools/fetch_ngx_headers.ps1`：从官方固定版本获取 NVIDIA 构建头文件并校验哈希。
- `tools/generate_optical_flow.py`：需要重新生成光流排列时使用的脚本。
- `docs/images`：已查看的两张单次运行截图。
- `licenses`：随二进制和源码需要保留的许可原文。

## 打包时的最小布局调整

原始稳定插件来自 `mgpu_prism_x1` 基线。发布副本把 AMD 的 CMake 路径从开发机的 sibling 目录改到 `third_party/amd_fidelityfx`，并用最小 `PIX_COLOR` 兼容定义替代原开发布局中 Magpie include 目录提供的唯一编译宏。不包含 Microsoft PIX 头文件、PIX 运行库或 Magpie 源码；没有为发布改写项目原有的 `.cpp`、`.hpp`、HLSL 或生成头文件。

## 明确不包含

- `_nvngx.dll`、`nvngx_dlssnr.dll` 和其他 NVIDIA 二进制、模型；
- NVIDIA 与 Intel 的源码头文件，按各自许可由构建者准备；
- ReShade 安装器或运行时；
- 游戏可执行文件、游戏素材、用户存档、账号信息；
- 私人绝对路径、运行日志、崩溃转储；
- 构建目录、缓存、IDE 文件；
- 撤回的窗口实验二进制；
- 未核验可再分发许可的第三方整包源码。

## 运行包内容

运行包独立提供稳定 `nvngx.dll_mgpu_bridge.addon64`、中文 `mgpu.ini`、`gpu1.ini`、`ReShade2.ini`、深度 tap、原样 `libxess_fg.dll`／`libxell.dll`、中文 `README.txt` 和许可。完整文件级哈希在打包后生成。
