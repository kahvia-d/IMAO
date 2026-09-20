[简中](README.md) | [EN](README.en.md) 

### 介绍
一款叠加在游戏窗口上的交互地图，借助图像匹配技术，实时同步玩家位置，减少玩家在探索过程中来回切换游戏与地图工具的操作次数。

本项目在 [IMao-Wuthering-Waves](https://github.com/Yepin2022/IMao-Wuthering-Waves) 基础上修复和二次开发，地图、点位等游戏数据来自[库街区鸣潮大地图](https://www.kurobbs.com/mc/map/)。当前支持范围、已修复问题和未完成的验收见[项目审计报告（2026-09-07）](Docs/ProjectAudit_20260907.md)。本次架构调整与验收记录见[核心重构记录](Docs/Refactor_20260907.md)。

### 功能演示
<details>
  <summary>小地图导航</summary>
  <img src="https://github.com/user-attachments/assets/058fec38-70c2-4fb9-9be5-4594970c7dce"/>
</details>

<details>
  <summary>交互式大地图</summary>
  <img src="https://github.com/user-attachments/assets/22ba7107-3640-4fc3-9a25-f030ab5106ef"/>
</details>

<details>
  <summary>路线指引</summary>
  <img src="https://github.com/user-attachments/assets/765c7e9b-bb05-46a7-8ece-64e5ba67ce27"/>
  <img src="https://github.com/user-attachments/assets/b49999de-c616-4c09-921b-7658eed0085a"/>
</details>

### 使用方法
1.  运行环境准备 [NET 8.0](https://dotnet.microsoft.com/en-us/download/dotnet/8.0) 和 [Microsoft Visual C++ 2015-2022 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170)
2.  按[构建说明](Docs/Build_zh-Hans.md)生成当前版本，运行输出目录中的 IMao-WinUI.exe。原上游 Releases 不包含本项目的后续修复。
3.  打开游戏，并将其分辨率调成16:9
4.  在确保游戏左下角坐标清晰显示后单击启动按钮，等待右下角信息消失后，即成功识别到正确的坐标后，在功能页开启需要的功能即可

### 项目依赖（包括但不限于）
* [OpenCV](https://github.com/opencv/opencv)
* [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR)
* [Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample)
* [ImGui](https://github.com/ocornut/imgui)
* [nlohmann/json](https://github.com/nlohmann/json)
* [CommunityToolkit/Windows](https://github.com/CommunityToolkit/Windows)
* [microsoft-ui-xaml](https://github.com/microsoft/microsoft-ui-xaml)

### 开发
[如何编译项目](Docs/Build_zh-Hans.md)

全部设计、审计与历史记录见[文档索引](Docs/README.md)（`Docs/` 下按构建、更新、地图数据、界面、审计分类，一次性记录归档在 `Docs/archive/`）。
