# 阻塞与非阻塞 IO

一句话：阻塞 IO 没数据就"卡住等"；非阻塞 IO 没数据就"立刻返回 -1 + EAGAIN"。

## 区别
| | 阻塞 | 非阻塞 |
|---|---|---|
| 没数据时 | 线程卡住 | 立刻返回 -1，errno = EAGAIN |

## 设置非阻塞
```cpp
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);   // 先读原标志，再或上
```

## 为什么重要
- 非阻塞不能单独用（会忙等烧 CPU）
- 必须配合 [[select-poll-epoll]] 一起

## 关联
[[LT与ET]]（ET 必须配非阻塞）、[[select-poll-epoll]]
