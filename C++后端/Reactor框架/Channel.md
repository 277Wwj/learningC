# Channel

一句话：一个 fd 的"代言人"——绑定 fd + 事件 + 回调。

## 职责
- `fd`、`events`（订阅什么事件）、四个回调（读/写/关闭/错误）
- `handleEvent(revents)`：按位运算分发到对应回调

## 关键
- `events_` 是订阅，`revents` 是实际发生
- 回调是 `std::function<void()>` 盒子，由使用者塞进 lambda（具体逻辑使用者决定）

## 坑
不能在回调里 delete 自己 → [[use-after-free]]

## 关联
[[Poller]]、[[EventLoop]]、[[Buffer]]
