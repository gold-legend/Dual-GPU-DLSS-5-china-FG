# 手动发布步骤

这份流程只供你验收后手动操作。当前文件制作过程没有登录 GitHub、没有创建仓库或 Release，也没有上传任何内容。

## 一、先验收本地文件

1. 阅读 `README.md`，确认功能、性能和限制表述符合你的意图。
2. 打开 `RESULTS.md` 中两张截图，确认允许公开，截图没有账号、路径或其他隐私。
3. 检查运行包，确认不包含 `_nvngx.dll`、`nvngx_dlssnr.dll`、其他 NVIDIA DLL、游戏文件或私人日志。
4. 对照 `SHA256SUMS.txt` 检查压缩包完整性。
5. 重新阅读 `THIRD_PARTY.md` 和 `licenses`，发布者需要自行确认官方许可义务，尤其是 NVIDIA SDK 的条款。

## 二、创建仓库

在 GitHub 手动新建公开仓库，例如命名为 `MGPU-Prism-FG`。名字只是建议，本文没有替你预设或占用地址。

建议仓库简介：

```text
双卡神经渲染、固定1.5帧编辑场对齐与XeSS X2输出级帧生成。基于MGPU 0.4与Prism NR思路的社区二次开发。
```

建议主题：

```text
mgpu, prism-nr, xess-fg, optical-flow, dual-gpu, reshade, dlss-nr
```

将“源码与文档包”中的**目录内容**上传到仓库根目录，确保首页能看到 `README.md`、`LICENSE`、`src`、`docs` 和 `third_party`。不要把本地绝对路径、构建缓存、日志、游戏文件或撤回实验二进制传上去。

如果采用 Git 网页上传，文件夹较多时可分批拖拽；上传后检查子目录是否完整。不要把源码 ZIP 当成仓库唯一内容，否则访问者无法直接阅读代码。

## 三、创建 Release

1. 打开仓库的 **Releases**，选择 **Draft a new release**。
2. 新建标签，例如 `v1.0.0`；目标分支选择你实际保存 1.0 源码的分支。
3. 标题填写：`MGPU Prism FG 1.0`。
4. 将 `RELEASE_NOTES.md` 的正文粘贴到 Release 描述，不需要粘贴文件顶部重复标题也可以。
5. 上传以下三个文件：

```text
MGPU-Prism-FG-1.0-运行包.zip
MGPU-Prism-FG-1.0-源码与文档.zip
SHA256SUMS.txt
```

6. 如果仍希望明确标记测试性质，可选择 **Set as a pre-release**；不要在未完成验收时写“全游戏稳定版”。
7. 保存为草稿，先检查附件下载和首页图片，再手动发布。

## 四、发布后自检

- 仓库首页是否能显示两张实测图；
- `README.md` 中所有相对链接是否可打开；
- Release 是否能下载三个附件；
- 新环境解压运行包后，目录结构是否与 `INSTALL.md` 一致；
- 仓库源码是否包含 NVIDIA／Intel 源码头文件、NVIDIA DLL 或私人文件；
- 许可、作者归属与“非官方分支”说明是否保留；
- 是否把显示输出 FPS 误写成神经 FPS 或普遍性能翻倍。

如发现问题，先撤下有误附件或把 Release 转回草稿，再修正；不要在评论区用一句“以实际为准”替代修正文档。
