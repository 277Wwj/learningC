# EventLoop

一句话：事件循环，总调度——反复"等事件 → 分发到 Channel 的回调"。

## 职责
- 持有 [[Poller]]
- `loop()`：while(running) { poll() → handleEvent() }
- `quit()`：退出循环（优雅关闭时用）

## 代码骨架
```cpp
while (running_) {
    auto ready = poller_.poll();
    for (auto& [ch, revents] : ready) ch->handleEvent(revents);
}
```

## 关联
[[Poller]]、[[Channel]]、[[timerfd]]、[[WebServer]]
