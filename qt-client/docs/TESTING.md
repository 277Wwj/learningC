# LearningC Studio 交付测试记录

本文件保留 1.0.0/1.0.1 历史验证。1.0.2 上传前的构建、回归和远端基线见 [PUBLISH.md](PUBLISH.md)。

首次验证日期：2026-10-02；1.0.1 回归验证：2026-10-06。此记录区分客户端测试、界面检查和真实服务端联调。最新 TCP 代理修复及真实 Linux 连接结果见 [PROXY-FIX.md](PROXY-FIX.md)。

## 环境与构建

- Windows x64，Qt 6.8.3 MSVC 2022 64 位动态库。
- Visual Studio 2022，MSVC 19.41，C++17，CMake，Release 构建。
- 独立配置 `qt-client/CMakeLists.txt`；不构建 Linux epoll 服务端，不依赖原工程 GTest。
- 本地服务端源码基线：GitHub `277Wwj/learningC` 的 `dc9f75193d9b8dbf2914d25609cc47c4ab66b2dd`。首次工作区通过源码快照建立；当时未修改服务端，也未向 GitHub 推送。此次上传以远端最新提交为基线，见 PUBLISH.md。

实际执行入口：

```powershell
.\qt-client\scripts\build.ps1 -QtPath 'F:\codexproject\.qt\6.8.3\msvc2022_64' -Configuration Release
.\qt-client\scripts\package.ps1 -QtPath 'F:\codexproject\.qt\6.8.3\msvc2022_64'
```

构建过程中的 `WrapVulkanHeaders` 提示是 Qt 可选依赖探测；本程序使用 Qt Widgets/QPainter，不使用 Vulkan。

## 自动化测试

四组 CTest 全部通过，共 39 个具体测试用例；Qt Test 加上每组初始化和清理后合计 47 项通过、0 失败。每组的 Qt Test 明细保存在构建目录 `test_*-results.txt`；本次交付副本见 [test-results](test-results/)。测试使用本机临时端口，没有依赖外部业务服务；新增代理回归使用本机非回环 IPv4 地址，验证连接、帧发送与重连，在没有此类网卡的环境会跳过（本次未跳过）。测试程序不进入应用发布包。

| 测试组 | 验证内容 | 结果 |
| --- | --- | --- |
| `test_http` | 完整 200、404、手动重定向、200 后中途断流；请求正文及头、HEAD；连接拒绝、超时、取消后重发；响应大小限制；输入校验；完成信号重入及活动请求销毁 | 通过 |
| `test_benchmark` | 并发 1/4/32；延迟服务端实际观察到的活动连接数；请求次数与连接总数；每请求独立连接；2xx/HTTP 错误/断流/超时混合；停止和重启；CSV 行列及汇总一致；运行中销毁 | 通过 |
| `test_tcp` | 与固定字节向量核对大端编码；无 Welcome 时保持连接；分段、连续两帧发送；半包/粘包接收；最大负载、多帧；非法长度/魔数；远端关闭/拒绝；断开排空；取消尾段与重连隔离；发送队列上限 | 通过 |
| `test_ui` | Windows 原生窗口实际点击；JSON/响应头/404/历史重放/取消；压测完成与停止；TCP 构帧、Chat 解析及未知 uint32 类型导出；设置 `/api` 路径保留；小窗口导航与滚动 | 通过 |

压测成功仅统计完整 2xx；延迟分位数仅使用成功请求。停止后的取消请求单列，未发起请求不计为取消。测试验证客户端统计口径，不代表服务端性能成绩。

## 界面与缩放

界面测试使用 `-platform windows`，中文字体在本机原生窗口检查。截图包括响应、压测、TCP 收发、空状态和 1024×600 逻辑像素小窗口；窄或矮窗口通过内容滚动保留编辑区和操作入口。

以本机原生 125% 缩放为基线，分别设置 `QT_SCALE_FACTOR=0.8 / 1 / 1.2`，取得有效 100% / 125% / 150% 渲染。1320×860 逻辑像素窗口对应截图为 1320×860、1650×1075、1980×1290 物理像素。此方法使用 Qt 的测试用比例因子，不修改用户系统显示设置，依据 [Qt 6.8 高 DPI 文档](https://doc.qt.io/qt-6.8/highdpi.html#qt-scale-factor)。

- [HTTP 请求与响应](screenshots/01-http.png)
- [压测结果](screenshots/02-benchmark.png)
- [TCP 收帧解析](screenshots/03-tcp-received.png)
- [小窗口滚动布局](screenshots/04-small-window.png)
- [100%](screenshots/dpi-100.png) / [125%](screenshots/dpi-125.png) / [150%](screenshots/dpi-150.png)

响应与曲线截图来自自动化测试夹具；夹具中的 JSON、Welcome/Chat 等数据只证明客户端展示能力，不代表最新聊天后端已经完成同样的运行验证。

## 发布包

打包脚本使用 `windeployqt`，附带 Qt Core/Gui/Widgets/Network、Windows 平台插件、app-local MSVC x64 CRT、中文文档与许可说明。发布目录和 ZIP 均由脚本生成。

独立发布目录启动结果与实际依赖路径见 [发布检查](PACKAGE-CHECK.md)。本机启动检查不等于在一台全新、未安装开发工具的 Windows 机器上完成验证。

## 真实后端联调：待执行

首次交付时本机没有可用 WSL/Linux 运行环境，也没有可访问的 Linux 服务地址。2026-10-06 已连接用户的 VirtualBox Linux，进行了 HTTP、回声与 TCP 连接的部分检查；TCP 代理修复验证见 PROXY-FIX.md。以下完整验收清单仍需逐项验证，不能用测试夹具结果替代：

| 真实程序 | 待验证步骤 |
| --- | --- |
| `webserver` | `/`、`/hello` 返回 200，未知路径返回 404 |
| `webserver_thread` | `/`、`/slow`、未知路径；慢请求超时与手动取消 |
| `webserver_multithread` | 不同路径均返回固定 200；同参数短连接测试 |
| `chat_server` | 12735 连接；双客户端核对 Welcome/Users、群聊/私聊 Chat、重复登录 Error、Bye 关闭，以及分段/连续帧 |

客户端中三个 HTTP 版本名称仅是用户选择的配置标签，不自动识别或切换远端进程。三个程序共用 8080，需逐个启动。TCP 当前只确认连接、帧入队和收到响应，不显示虚构登录/送达状态。完整操作步骤见 [DEMO.md](DEMO.md)。

另外尚未覆盖跨显示器动态 DPI 切换、Linux 桌面构建、干净 Windows 虚拟机，以及长时间容量测试。它们不是本次客户端自动化测试的结论。
