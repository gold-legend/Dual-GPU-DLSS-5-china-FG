# MGPU Prism FG 2.0 发布与核验步骤

## 一、发布前验收

1. 阅读 `README.md` 与 `RESULTS.md`，确认功能、性能与限制表述和实际证据一致。
2. 检查运行包，确认不包含 `_nvngx.dll`、`nvngx_dlssnr.dll`、`nvngx_dlssg.dll`、其他 NVIDIA DLL、游戏文件或私人日志。
3. 从源码包独立配置、构建并运行自动测试；不要把原开发目录构建成功当成发布副本已验证。
4. 对照 `SHA256SUMS.txt` 检查压缩包完整性，并核对运行包插件哈希与 Release 构建产物一致。
5. 阅读 `THIRD_PARTY.md` 和 `licenses`，保留原版权与许可。
6. 将 Release 标为预发布，直到 PresentMon 对照、神经开关实机复验、自定义分辨率和长时间测试完成。

## 二、源码与标签

- 将发布源码与文档保存到仓库根目录，确保首页能直接阅读。
- 2.0 使用标签 `v2.0.0`，标题为 `MGPU Prism FG 2.0`。
- 不覆盖或改动 `v1.0.0` 历史标签。
- 不把源码 ZIP 当成仓库唯一内容，也不上传构建缓存和本地运行日志。

## 三、Release 附件

```text
MGPU-Prism-FG-2.0-Runtime.zip
MGPU-Prism-FG-2.0-Source.zip
SHA256SUMS.txt
```

Release 正文使用 `RELEASE_NOTES.md`。明确保留：PresentMon 本次采样失败、仍有轻微运动闪烁、自定义分辨率上调失效、神经开关交接尚待实机复验。

## 四、发布后自检

- 仓库首页和文档相对链接可打开；
- Release 标签指向 2.0 提交，三个附件均可下载；
- 下载后的哈希与 `SHA256SUMS.txt` 一致；
- 新环境解压运行包后，目录结构与 `INSTALL.md` 一致；
- 仓库和附件不包含 NVIDIA 二进制、SDK 头文件、私人路径或日志；
- 许可、作者归属与非官方分支说明保留；
- 不把 Present 输出 FPS 误写成真实显示 FPS、神经求值 FPS 或普遍性能翻倍。

如发现问题，先撤下有误附件或把 Release 转回草稿，再修正文档和二进制；不要用一句“以实际为准”替代修正。
