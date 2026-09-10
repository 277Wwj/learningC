# timerfd

一句话：把"定时"变成"fd 到点可读"，无缝融入 epoll/Reactor。

## 核心洞察
到点 → fd 变可读 → epoll 唤醒 → 执行超时处理。

## 三个 API
`timerfd_create` → `timerfd_settime` → `read`（必须读走 8 字节，否则疯狂触发）

## 用途
[[WebServer]] 里实现连接超时断开（每 5 秒扫描 lastActive 超过 10 秒的连接）

## 关联
[[EventLoop]]、[[Channel]]、[[WebServer]]
