# WebServer 项目

一句话：epoll + Reactor 手写的高并发 HTTP 服务器。单线程 5.0 万 QPS，主从 5.3 万 QPS（wrk 长连接 / Release / 5 核虚拟机）。

## 三个版本（演进路线）
1. **单线程版** `webserver.cpp`：一个 [[EventLoop]] 处理所有连接（wrk 长连接 5.0 万 QPS）
2. **线程池版** `webserver_thread.cpp`：连接分给 [[线程池]] 处理
3. **主从 Reactor 版** `webserver_multithread.cpp`：一主多从，轮询分发，多核并行（raw epoll 简化版；已支持 [[keep-alive]]，wrk 5.3 万 QPS）

## 组成
[[EventLoop]] + [[Poller]] + [[Channel]] + [[Buffer]] + [[timerfd]] + eventfd

## 功能
- HTTP GET 解析、[[keep-alive]]、404
- 连接超时断开（[[timerfd]]）
- 优雅关闭（eventfd + 信号）
- 主从 Reactor：主线程 accept，轮询分发给 4 个从 reactor

## 压测（2026-09-28，Release 构建，5 核虚拟机）

| 版本 | 工具 / 模式 | QPS |
|---|---|---|
| 单线程 | wrk / 长连接 | **50,106** |
| 单线程 | ab -k / 长连接 | 11,108（ab 自己先到顶：单核 94%） |
| 主从（改造前） | wrk / 短连接 | 14,023 |
| 主从（keep-alive 后） | wrk / 长连接 | **52,802** |

结论（面试素材）：
- **压测工具是第一个瓶颈**：ab 单进程 ~1.1 万/核，wrk ~2.6 万/线程；换 wrk 后单线程直接 5 万
- **主从版"慢"的真相**：改造前写死 `Connection: close`（短连接，每请求一次握手）；补 keep-alive 后 1.4 万 → 5.3 万
- 两版打平在 ~5 万量级，且都可能是"喂食不足"（wrk 2 线程也接近上限）；测真实上限需两台机器
- **验证长连接生效**：压测中 `ss -tan | grep 8080 | wc -l` ≈ 200（100 连接 × 两端）
- 教训：**报 QPS 必须带条件**（工具 / 模式 / 构建类型 / 机器），否则无法比较、无法复现

## 踩过的坑（面试重点）
1. HTTP/1.0 不关连接 → QPS 从 6 暴跌到 3.7 万（修复后）
2. 析构锁内 join → [[mutex与死锁]]
3. packed 结构体 emplace_back 引用绑定失败
4. use-after-free：回调里不能 delete 自己（延迟删除/智能指针）
5. 单机自压自测：工具和服务器抢核；4 个 ab 打 5 核机器 → 过载反噬，总吞吐反降
6. 主从版 `listen(fd, 10)` backlog 太小 → 短连接风暴下排队溢出（已改 1024）
7. 排查手法：`top -H -p <pid>` 看线程级 CPU；先怀疑工具，再怀疑代码

## 待升级
- [ ] 主从版：半包/粘包加固（每连接 Buffer，参考 [[Buffer]] 与 [[粘包与半包]]）
- [ ] 主从版：空闲连接超时清理（[[timerfd]]）
- [ ] 定时器改时间轮/最小堆
- [ ] 异步日志（双缓冲）
- [ ] POST + 请求体解析
- [ ] shared_ptr 管理 Channel 生命周期

## 单元测试（2026-10-01）
- `tests/buffer_test.cpp` + `tests/eof.cpp`（GTest，挂在 CMake 上；跑法：`ctest` 或 `./build/buffer_test`）
- 6 个用例：初始状态 / 读取+取出 / **取多了（边界）** / 两次写入累积 / 半取再全取 / **对端关闭（EOF）**
- 测出来的语义：`readFd` 遇到对端关闭时，**即使本轮读到过数据也返回 -1**；数据还在 buffer 里，要靠 `readableBytes()` 判断

## 关联
完整文档见项目根目录 README.md
