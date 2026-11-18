# PresentMon 显示帧测量

本目录只包含采样与分析脚本，不附 PresentMon 二进制。请从 Intel／PresentMon 官方发布页获取签名工具，并将 `PresentMon-2.6.0-x64.exe` 放到本目录后再使用 `capture.bat`。

测量帧生成时必须区分：

- `generated`：生成器产出；
- `presented-gen`：生成帧调用 Present；
- `MsBetweenDisplayChange`：PresentMon 跟踪到实际显示变化的间隔。

`generated` 或 `presented-gen` 不能单独证明真实上屏。FG 开启与关闭应在同机位、同分辨率、同画质下分别采样，并按 `SwapChainAddress` 分开统计。DcompOverlay 下可能无法单独看到桥接链，这时不能拿游戏原始链代替神经链结论。

`latcmp.py` 用法：

```powershell
python latcmp.py BASE.csv TEST.csv
```

它同时输出交换链、显示覆盖率和帧率信息。不要把单个总 FPS 当作帧生成验收。
