# WebServer —— 基于 epoll + Reactor 的高并发 HTTP 服务器

> 从零手写的轻量级 C++ HTTP 服务器，使用 epoll（ET 边缘触发）+ Reactor 架构，
> 支持长连接、连接超时管理、404 处理与优雅关闭。**单线程压测 QPS 达 3.7 万。**

## 功能特性

- **IO 多路复用**：epoll（ET 边缘触发）+ 非阻塞 IO
- **Reactor 架构**：自研 `EventLoop` / `Poller` / `Channel` 三层框架
- **HTTP/1.1 支持**：GET 请求解析、keep-alive 长连接、404 处理
- **连接管理**：连接超时自动断开（timerfd 定时扫描）
- **优雅关闭**：Ctrl+C 安全退出（eventfd 唤醒事件循环）
- **性能**：单线程 36927 QPS，零失败，P99 延迟 7ms

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

## 技术栈

- 语言：C++17
- IO：epoll、ET 边缘触发、非阻塞 socket
- 架构：Reactor（EventLoop / Poller / Channel / Buffer）
- 定时：timerfd、eventfd
- 工具：gdb、valgrind、strace、ab（压测）

## 压测数据

环境：虚拟机（VirtualBox，单线程 Reactor）

| 场景 | 命令 | QPS | 备注 |
|------|------|-----|------|
| 长连接 | `ab -k -n 100000 -c 100` | **36927** | P50=2ms，P99=7ms，0 失败 |
| 短连接 | `ab -n 100000 -c 100` | 7336 | 每请求含握手/挥手开销 |

## 编译与运行

```bash
# 编译
g++ -std=c++17 -I include src/webserver.cpp -o webserver

# 运行
./webserver

# 测试
curl http://127.0.0.1:8080/           # 首页
curl http://127.0.0.1:8080/hello      # Hello
curl http://127.0.0.1:8080/xyz        # 404

# 压测
ab -k -n 100000 -c 100 http://127.0.0.1:8080/
```

## 目录结构

```
include/
  Channel.hpp      # fd 的事件封装（回调分发）
  Poller.hpp       # epoll 封装
  EventLoop.hpp    # 事件循环
  Buffer.hpp       # 收发缓冲区
  Threadpool.hpp   # 线程池（早期版本）
  simpleLogger.hpp # 异步日志
src/
  webserver.cpp    # 主项目：HTTP WebServer
  server.cpp       # epoll echo 服务器
  lesson*.cpp      # 学习过程中的实验代码
```

## 踩过的坑（面试重点）

1. **析构死锁**：线程池析构时在持有锁的情况下 `join` worker，worker 被唤醒后需重新拿锁才能退出 → 双向等待死锁。解决：`join` 移出锁外。

2. **HTTP/1.0 性能灾难**：服务器对所有请求都返回 keep-alive，但 HTTP/1.0 客户端靠"连接关闭"判断响应结束，于是每个请求干等 10 秒直到超时断开，QPS 从 6 暴跌。解决：根据请求的 HTTP 版本决定是否 keep-alive，修复后 QPS 达 3.7 万。

3. **packed 结构体绑定引用失败**：`epoll_event` 是 packed 结构，其字段不能被 `emplace_back` 绑定成引用。解决：先拷贝到局部变量。

4. **use-after-free**：不能在 Channel 自己的回调里 `delete` 自己，否则回调返回后 `handleEvent` 继续访问已释放对象。解决方向：延迟删除（智能指针）。

5. **信号处理器限制**：信号处理器只能调用异步信号安全函数（如 `write`），不能调 `printf`/`malloc`。用 eventfd 唤醒事件循环，把复杂处理留给主循环。

## TODO（进阶方向）

- [ ] 主从 Reactor + 线程池（提升多核利用）
- [ ] POST 请求与请求体解析
- [ ] 静态文件服务
- [ ] MySQL 连接池
- [ ] 定时器改用时间轮/红黑树（支持海量连接）
