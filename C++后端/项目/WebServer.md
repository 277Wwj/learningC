# WebServer 项目

一句话：epoll + Reactor 手写的高并发 HTTP 服务器，单线程 QPS 3.7 万。

## 组成
[[EventLoop]] + [[Poller]] + [[Channel]] + [[Buffer]] + [[timerfd]] + eventfd

## 功能
- HTTP GET 解析、[[keep-alive]]、404
- 连接超时断开（[[timerfd]]）
- 优雅关闭（eventfd + 信号）

## 压测
- 长连接：36927 QPS，P50=2ms
- 短连接：7336 QPS

## 踩过的坑（面试重点）
1. HTTP/1.0 不关连接 → QPS 从 6 暴跌到正常
2. 析构锁内 join → [[mutex与死锁]]
3. packed 结构体 emplace_back 引用绑定失败
4. [[use-after-free]]（🕳 阶段四补：智能指针）

## 关联
完整文档见项目根目录 README.md
