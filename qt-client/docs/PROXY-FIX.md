# 1.0.1：TCP 连接代理错误修复

日期：2026-10-06。本文是 1.0.1 历史修复记录，截图和哈希不适用于后续构建；当前源码版本及验证见 [PUBLISH.md](PUBLISH.md)。

用户连接 VirtualBox Linux `192.168.1.111:12735` 时，客户端报 `The proxy type is invalid for this operation`。Windows 原生 TCP 连接成功；使用 Qt 默认 socket 在同一环境复现了该错误。问题发生在客户端选用代理的阶段，不是服务器端口未监听。

Qt socket 默认使用应用/系统代理选择。TCP 协议调试面向输入的具体服务器，现已在每次创建 socket 后显式设为直连：

```cpp
socket_ = new QTcpSocket(this);
socket_->setProxy(QNetworkProxy::NoProxy);
```

依据：[Qt 6.8 QAbstractSocket::setProxy](https://doc.qt.io/qt-6.8/qabstractsocket.html#setProxy)。此改动仅作用于 `TcpClient`，不修改操作系统代理，不改变 HTTP/压测的代理策略。不支持通过代理连接 TCP 调试目标。

## 验证

- 新增 `directConnectionIgnoresApplicationProxy` 回归：本机非回环 IPv4 监听临时端口，设置不支持原始 TCP 的 HttpCachingProxy；验证连接、Login 字节发送、断开及再次连接。原实现失败，修复后通过。
- 四组 CTest 全部通过：HTTP 14 项、压测 8 项、TCP 18 项、UI 7 项，共 47 项（含各组初始化/清理），39 个具体用例，0 失败、0 跳过。
- 真实网络检查：默认 Qt socket 对用户 Linux 端口报原始代理错误；使用修复后的 `TcpClient` 成功连接 `192.168.1.111:12735`，验证程序退出码 0。
- 本次真实 TCP 检查仅建立连接后断开，没有发送聊天业务消息；不代表后端已实现登录回复或消息转发。
- 重新打包后，在只保留 Windows 系统目录的 PATH 下启动发布版 TCP 页面，截图成功且退出码 0；[发布截图](screenshots/06-v1.0.1.png) 显示 v1.0.1。构建与发布 EXE 的 SHA-256 均为 `ECC8E94F1C17A8D02F14EBF9DE01BB52670F42872AC3B4ACCE7BC68612588D60`。

## 使用新版

关闭旧窗口，使用新发布目录内的 `learningc_studio.exe`，左下角版本应为 **v1.0.1**。TCP 页面服务器输入 `192.168.1.111`（不含 `http://`），端口输入 `12735`，点击连接。发送帧后如果提示“暂未收到完整响应帧”，连接仍可保持：旧后端可能仅记录帧；最新版对合法 Login 应返回 Welcome，实际行为需核对正在运行的服务端版本。

请求历史和未保存输入不会从正在运行的旧进程迁移；已保存的服务器配置继续使用原配置文件。
