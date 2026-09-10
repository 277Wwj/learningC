# Poller

一句话：封装 epoll，只负责 epoll_ctl 和 epoll_wait，不碰业务。

## 职责
- `updateChannel` / `removeChannel` → epoll_ctl
- `poll()` → epoll_wait，返回就绪 (Channel*, revents) 列表

## 关键设计
把 `Channel*` 指针存进 `epoll_event.data.ptr`，唤醒时直接拿回对象（免查表）。

## 关联
[[EventLoop]]、[[Channel]]、[[select-poll-epoll]]
