# MGPU Prism FG 2.1 仓库内容与发布清单

## 文档

- `README.md`：项目定位、2.1 功能、安装入口与已知问题。
- `ARCHITECTURE.md`：三遍串行、PX1、同帧绑定、DLSS FG 与 Present 交接。
- `RESULTS.md`：本机日志证据、PresentMon 失败原因与未完成的验收。
- `INSTALL.md`：中文安装、配置、快捷键和排障。
- `BUILD.md`：依赖布局、构建命令和复现边界。
- `THIRD_PARTY.md`：第三方归属、许可路径和不分发项。
- `RELEASE_NOTES.md`：GitHub Release 正文。
- `MANUAL_PUBLISH.md`：发布与发布后核验步骤。
- `CHANGELOG.md`：2.1 与 1.0 更新范围。

## 源码与资源

- `src`：2.1 插件完整源码，包括保留的研发诊断和旧路径。
- `ext/reshade`：ReShade 公开 add-on 头文件及许可。
- `third_party/minhook`：MinHook 源码与许可。
- `third_party/mgpu_native_input/include`：本分支 native-input ABI 头文件。
- `tests`：可选 CPU 侧测试与源码结构检查。
- `assets`：深度 tap、默认配置、副卡配置和安装说明。
- `tools/fetch_ngx_headers.ps1`：从官方固定版本获取并校验 NVIDIA 构建头文件。
- `tools/latency_tools`：PresentMon 采样与分析脚本，不附采样器二进制。
- `licenses`：随源码和二进制需要保留的许可原文。

## 打包布局调整

原始开发目录通过 sibling `Mgpu-native-input/include` 提供 native-input ABI。2.1 发布副本把同一个头文件放到 `third_party/mgpu_native_input/include`，并只修改 CMake 的引用路径，使仓库可以独立构建。没有为打包改写算法实现。

## 明确不包含

- `_nvngx.dll`、`nvngx_dlssnr.dll`、`nvngx_dlssg.dll` 和其他 NVIDIA 二进制、模型；
- NVIDIA SDK 源码头文件，按官方许可由构建者运行脚本准备；
- ReShade 安装器或运行时；
- 游戏可执行文件、游戏素材、用户存档、账号信息；
- 私人绝对路径、运行日志、崩溃转储；
- 构建目录、缓存、IDE 文件；
- 1.0 的 Intel／AMD 运行库与已移除的旧主路径；
- PresentMon 二进制，采样工具由测量者自行从官方来源获取。

## 运行包内容

运行包提供 `nvngx.dll_mgpu_bridge.addon64`、默认 `mgpu.ini`、`gpu1.ini`、`ReShade2.ini`、`panel_simple.txt`、深度 tap、中文 `README.txt` 和许可。用户自行添加 NVIDIA 模块。完整文件级哈希在打包后生成。

## 发布附件

```text
MGPU-Prism-FG-2.1-Runtime.zip
MGPU-Prism-FG-2.1-Source.zip
SHA256SUMS.txt
```
