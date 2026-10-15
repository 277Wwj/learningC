# Qt 许可文本与来源

本目录随客户端一起提供 Qt 使用的 LGPLv3 和 GPLv3 原文，未修改许可正文。文件下载自 [Qt 官方 qtbase 仓库的 v6.8.3 标签](https://github.com/qt/qtbase/tree/v6.8.3/LICENSES)，取得日期为 2026-10-02。

| 文件 | 官方原文 | SHA-256 |
| --- | --- | --- |
| `LGPL-3.0-only.txt` | [Qt LGPLv3](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/LGPL-3.0-only.txt) | `da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768` |
| `GPL-3.0-only.txt` | [Qt GPLv3](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/GPL-3.0-only.txt) | `8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903` |

客户端动态链接 Qt Core、Gui、Widgets 和 Network。Qt 的版权属于 The Qt Company Ltd. 及其贡献者；Qt 内部第三方组件保留各自版权与许可。许可选项、第三方声明入口见 [Qt 6.8 Licensing](https://doc.qt.io/qt-6.8/licensing.html)。这两份文本用于说明 Qt 的许可，不是在替本仓库作者选择或新增整个项目的许可证。

Qt 6.8.3 的对应源代码可从 [官方源代码归档](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/) 获取；本客户端使用的模块来自 `qtbase`。源码中的 `LICENSES`、各第三方组件声明及 SDK 的 SBOM 可用于核对实际部署组件。打包脚本会复制 SDK 内可找到的声明和 SBOM，并在 `QT-NOTICES.txt` 记录实际 SDK 版本与对应源码目录。

当前 SDK 中附带的少量 CMake 许可文件不能代替全部第三方声明。对外分发时，应按实际 Qt 版本、插件与依赖保留相应许可/版权信息，并落实所选许可证要求的源码、替换库和安装材料。替换为兼容修改版 Qt 库时，程序依赖 Qt 的共享库与插件目录，不嵌入静态 Qt。
