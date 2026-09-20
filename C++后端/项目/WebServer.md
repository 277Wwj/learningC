# WebServer 项目

一句话：epoll + Reactor 手写的高并发 HTTP 服务器，单线程 QPS 3.7 万。

## 三个版本（演进路线）
1. **单线程版** `webserver.cpp`：一个 [[EventLoop]] 处理所有连接，36927 QPS
2. **线程池版** `webserver_thread.cpp`：连接分给 [[线程池]] 处理
3. **主从 Reactor 版** `webserver_multithread.cpp`：一主多从，多核并行（[[主从Reactor]]）

## 组成
[[EventLoop]] + [[Poller]] + [[Channel]] + [[Buffer]] + [[timerfd]] + eventfd

## 功能
- HTTP GET 解析、[[keep-alive]]、404
- 连接超时断开（[[timerfd]]）
- 优雅关闭（eventfd + 信号）
- 主从 Reactor：主线程 accept，轮询分发给 4 个从 reactor

## 压测
- 长连接（单线程）：36927 QPS，P50=2ms，P99=7ms
- 短连接：7336 QPS
- 主从多线程：待补测（`ab -n 100000 -c 100`）

## 踩过的坑（面试重点）
1. HTTP/1.0 不关连接 → QPS 从 6 暴跌到 3.7 万（修复后）
2. 析构锁内 join → [[mutex与死锁]]
3. packed 结构体 emplace_back 引用绑定失败
4. use-after-free：回调里不能 delete 自己（延迟删除/智能指针）

## 待升级
- [ ] 定时器改时间轮/最小堆
- [ ] 异步日志（双缓冲）
- [ ] POST + 请求体解析
- [ ] shared_ptr 管理 Channel 生命周期

## 关联
完整文档见项目根目录 README.md
