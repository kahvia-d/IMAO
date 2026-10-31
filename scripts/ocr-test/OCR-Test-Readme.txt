旧电脑定位测试包

完整解压到新目录，不覆盖旧版。Start-OcrTest.ps1 必须与两个 CMD 和 IMao-Launcher.exe 放在同一目录。
启动脚本会请求管理员授权；取消授权时会明确报错。切换模式前，从托盘完全退出 IMao。

Start-CPU.cmd：CPU 对照模式。直接双击 IMao-WinUI.exe 也默认使用 CPU。
Start-GPU.cmd：尝试用 DirectML 加速坐标识别；不可用或推理出错时自动回退 CPU。
显卡编号默认 0，不保证是最快的一张。需要指定时，可在终端运行 Start-GPU.cmd 1 等。

同一游戏场景分别测试，每组约一分钟：
1. CPU，不录制；2. CPU，开启诊断录制；
3. GPU，不录制；4. GPU，开启诊断录制。
观察小地图标记是否闪烁、状态栏是否反复等待画面，以及游戏帧率/卡顿是否改变。
切换游戏大小地图、失焦再回来，也各试几次。

反馈时保留 Logs 和 Diagnostics 文件夹，并说明每组使用的模式、时间和显卡型号。
GPU 启动成功不代表整套识别都在 GPU 上；SURF 仍使用 CPU。
开始探索后，在“诊断”页确认“实际 DIRECTML”或“实际 CPU”；初始化完成前显示等待。
如果请求 DIRECTML 但实际为 CPU，请一并记录显示的回退原因。实际后端与回退原因也会写入日志中的 ocr-backend / coordinate-recognition。
本包保持原有标记时效，过期画面仍会隐藏标记，不保证所有低性能情形都完全消除闪烁。
