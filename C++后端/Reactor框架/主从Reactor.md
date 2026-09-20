# 主从 Reactor（多线程）

一句话：主 Reactor 只负责 accept，从 Reactor（多个）各带一个 epoll + 线程负责读写——把单线程 Reactor 的多核能力榨出来。

## 为什么需要
单线程 Reactor 的 [[EventLoop]] 所有连接共用一个 epoll 和一个线程，
CPU 多核时只有一个核在干活 → 升级成"一主多从"。

## 结构
```
主线程（main reactor）
   └── accept 新连接，round-robin 分发给某个从 reactor
从线程 0（sub reactor）── 自己的 epoll + 线程，处理一批连接的读写
从线程 1（sub reactor）
从线程 2（sub reactor）
从线程 3（sub reactor）
```

## 核心技巧
- 主线程 accept 到的 fd，通过 `epoll_ctl(ADD)` 加进**别的线程**的 epoll ——
  **epoll_ctl 是线程安全的**，这是跨线程"派活"的关键。
- 每个从 reactor 一个 [[EventLoop]] + 一个 [[Poller]]，连接分配后不再迁移（避免锁）。
- 用 `next % SUB_NUM` 轮询分配，保证负载均匀。

## 关联
[[EventLoop]]、[[Poller]]、[[Channel]]、[[线程池]]、[[WebServer]]
