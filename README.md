# learningC：Linux C++ 网络服务与 Qt 调试客户端

本仓库记录 C++ 网络编程学习过程，包含三个 HTTP 服务端版本、LCH1 聊天中转服务，以及独立的 **[LearningC Studio Qt 客户端](qt-client/README.md)**。部分实现由 AI 辅助完成；功能是否可靠以源码、测试与实际联调结果为依据。

## 项目入口

| 模块 | 运行环境 | 功能与入口 |
| --- | --- | --- |
| HTTP 服务端 | Linux / C++17 | 单 Reactor、线程池、主从 Reactor；端口 8080，详见下文 |
| 聊天服务端 | Linux / C++17 | `src/chat_server.cpp`，端口 12735；登录、在线名单、群聊、私聊与退出 |
| LearningC Studio 1.0.2 | Qt 6 / C++17；已在 Windows 构建验证 | [HTTP 调试、轻量并发测试、TCP 协议帧观察](qt-client/README.md) |
| 测试与说明 | Linux / Windows 分开执行 | [联调步骤](qt-client/docs/DEMO.md)、[客户端架构](qt-client/docs/ARCHITECTURE.md)、[客户端测试记录](qt-client/docs/TESTING.md)、[本次上传记录](qt-client/docs/PUBLISH.md) |

Windows 构建客户端只需配置 `qt-client/CMakeLists.txt`，不编译 Linux epoll 工程。客户端连接已经启动的 Linux 服务，不能替代服务端；跨机器填写 Linux 的可达 IP，Windows 上的 `127.0.0.1` 指 Windows 自己。

```powershell
# 在仓库根目录执行；改成自己的 Qt SDK 路径。
.\qt-client\scripts\build.ps1 -QtPath 'C:\Qt\6.8.3\msvc2022_64'
# 生成包含依赖的 Windows 运行包（输出到 dist，不提交构建产物）。
.\qt-client\scripts\package.ps1 -QtPath 'C:\Qt\6.8.3\msvc2022_64'
```

客户端 TCP 页面是协议调试工具：手动发送 Login 后检查 Welcome/Error，可开两个进程验证 Say → Chat。它没有联系人会话界面，也不把 TCP 建连当作登录成功。[协议及演示步骤](qt-client/docs/DEMO.md#实验四tcp-登录名单和消息转发)

以下保留原 HTTP 项目的架构说明与历史测量。**历史 wrk 数据不是本次重新测得的结果；Qt 客户端逐请求新连接的测量不能与它直接比较。** HTTP 行为按不同程序区分：`webserver` 提供 `/`、`/hello` 和 404；`webserver_thread` 提供 `/`、`/slow` 和 404；`webserver_multithread` 对不同路径返回固定 200。

## HTTP 服务端概览

> C++ HTTP 服务器学习实现：epoll + Reactor 架构，三个演进版本
> （单线程 → 线程池 → 主从 Reactor），支持 keep-alive、粘包/半包处理、连接超时、优雅关闭。
> **wrk 压测：单线程 5.0 万 QPS，主从 5.3 万 QPS。**

## 功能特性

- **IO 多路复用**：epoll + 非阻塞 IO；当前共享 `Channel` 使用 LT，线程池与主从版直接使用 epoll 的部分设置 ET，需按源码区分
- **三种演进版本**：单线程 → 线程池 → 主从 Reactor（一主多从，多核并行）
- **Reactor 架构**：自研 `EventLoop` / `Poller` / `Channel` / `Buffer` 框架
- **HTTP/1.1 支持**：GET 请求解析、keep-alive 长连接、404 处理
- **粘包/半包处理**：每连接独立缓冲区，循环提取完整请求
- **连接管理**：连接超时自动断开（timerfd 定时扫描）
- **优雅关闭**：Ctrl+C 安全退出（eventfd 唤醒事件循环）
- **工程化**：CMake 多目标构建 + GTest 单元测试（6 用例）

## 架构

```
主线程 EventLoop（事件循环）
   ├── Poller（封装 epoll，负责 epoll_ctl / epoll_wait）
   │     ├── 监听 Channel   —— accept 新连接
   │     ├── 连接 Channel   —— 每个客户端一个，处理 HTTP 请求
   │     ├── 定时 Channel   —— timerfd，每 5 秒扫描超时连接
   │     └── 唤醒 Channel   —— eventfd，响应 Ctrl+C 优雅退出
   └── Buffer（每个连接的收发缓冲区，解决 TCP 粘包/半包）
```

主从 Reactor 版（`webserver_multithread.cpp`）：

```
主线程：accept 新连接 → 轮询分发给 4 个从 Reactor
4 个从线程：各自一个 epoll，独立处理分到的连接（各管各的连接表，无需加锁）
```

## 技术栈

- 语言：C++17
- IO：epoll、LT/ET 触发模式、非阻塞 socket
- 架构：Reactor（EventLoop / Poller / Channel / Buffer）、主从 Reactor（多线程）
- 定时：timerfd、eventfd
- 工具链：CMake、GDB、GTest、valgrind、strace、wrk / ab（压测）

## 压测数据

环境：VirtualBox 虚拟机（5 核），Release 构建，wrk 2 线程 / 100 连接

| 版本 | 工具 / 模式 | QPS |
|------|------------|-----|
| 单线程 | wrk / 长连接 | **50,106** |
| 主从 Reactor | wrk / 长连接 | **52,802**（补 keep-alive 后） |
| 单线程 | ab -k / 长连接 | 11,108（ab 自身先到瓶颈：单核 94%） |

压测得到的结论（也是很好的排查教学）：
- `ab` 是单进程工具，单核只推得动 ~1.1 万 QPS，会**先于服务器到顶**；换 `wrk` 后单线程版直接 5 万——**先确保压测工具不拖后腿**
- 主从版最初只有短连接（每请求一次 TCP 握手），QPS 被锁在 1.4 万；补上 keep-alive 后到 5.3 万
- 单机自测存在调度抖动，绝对值仅作参考，**同条件下的对照实验才有意义**

## 编译与运行

```bash
# 构建（Release）
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j

# 运行（三个版本任选其一）
./build-release/webserver                # 版本 1：单线程
./build-release/webserver_thread         # 版本 2：线程池
./build-release/webserver_multithread    # 版本 3：主从 Reactor

# 接口测试
curl http://127.0.0.1:8080/           # 首页
curl http://127.0.0.1:8080/hello      # Hello
curl http://127.0.0.1:8080/xyz        # 404

# 单元测试
ctest --test-dir build-release --output-on-failure

# 压测
wrk -t2 -c100 -d20s http://127.0.0.1:8080/
```

## 目录结构

```
CMakeLists.txt              # 构建配置（4 个可执行文件 + 1 个测试目标）
include/
  EventLoop.hpp / Poller.hpp / Channel.hpp   # Reactor 框架三件套
  Buffer.hpp                                 # 收发缓冲区（含单元测试覆盖）
  Threadpool.hpp / simpleLogger.hpp / Trace.hpp
src/
  webserver.cpp             # 版本 1：单线程 Reactor
  webserver_thread.cpp      # 版本 2：线程池
  webserver_multithread.cpp # 版本 3：主从 Reactor
  chat_server.cpp           # 衍生项目：聊天中转服务器
tests/
  buffer_test.cpp / eof.cpp # Buffer 单元测试（GTest）
  chat_smoke_test.py        # 聊天业务冒烟测试（需要先启动 chat_server）
qt-client/
  CMakeLists.txt            # 独立 Qt 客户端工程
  scripts/                 # Windows 构建与部署脚本
  docs/                    # 架构、操作步骤、测试记录和截图
```

## 踩过的坑（面试重点）

1. **析构死锁**：线程池析构时在持有锁的情况下 `join` worker，worker 被唤醒后需重新拿锁才能退出 → 双向等待死锁。解决：`join` 移出锁外。

2. **HTTP/1.0 性能灾难**：服务器对所有请求都返回 keep-alive，但 HTTP/1.0 客户端靠"连接关闭"判断响应结束，于是每个请求干等 10 秒直到超时断开，QPS 从 6 暴跌。解决：根据请求的 HTTP 版本决定是否 keep-alive，修复后 QPS 达 3.7 万。

3. **packed 结构体绑定引用失败**：`epoll_event` 是 packed 结构，其字段不能被 `emplace_back` 绑定成引用。解决：先拷贝到局部变量。

4. **use-after-free**：不能在 Channel 自己的回调里 `delete` 自己，否则回调返回后 `handleEvent` 继续访问已释放对象。解决方向：延迟删除（智能指针）。

5. **信号处理器限制**：信号处理器只能调用异步信号安全函数（如 `write`），不能调 `printf`/`malloc`。用 eventfd 唤醒事件循环，把复杂处理留给主循环。

6. **主从版遗漏 keep-alive**：写死 `Connection: close`（每请求一次 TCP 握手），QPS 被锁在 1.4 万；补上连接复用后到 5.3 万。教训：**比较性能前先确认测试条件一致**。

7. **粘包/半包处理**：只 `recv` 一次就当作完整请求 → 半包被错杀（直接断连）、粘包丢数据。解决：每连接一个 `Buffer`，循环提取完整请求；关闭连接时同步清空缓冲区表（fd 会被内核复用，漏清理会出现"幽灵数据"）。

8. **accept 队列太小**：`listen(fd, 10)` 在短连接风暴下排队溢出，导致连接超时。改为 1024。

## TODO（进阶方向）

聊天服务的后续验证重点：数据与 EOF 同时到达的处理、转发后的载荷上限、慢客户端积压限制。此列表是待验证/改进事项，不代表本次上传已修复后端。

- [ ] 主从版：连接超时清理（timerfd）
- [ ] POST 请求与请求体解析
- [ ] 定时器改用时间轮 / 最小堆（支持海量连接）
- [ ] 异步日志（双缓冲）
- [ ] 基于现有 Reactor 框架的 RPC / KV 存储项目
