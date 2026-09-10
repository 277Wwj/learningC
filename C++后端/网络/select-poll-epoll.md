# select / poll / epoll

一句话：三者都是"一个线程同时监视多个 fd"的 IO 多路复用机制，epoll 是最优解。

## select 的三个局限
1. 最多 1024 个 fd（fd_set 是 1024 位位图）
2. 每次调用全量拷贝 fd 集合到内核
3. 返回后要遍历所有 fd 才知道谁就绪

## epoll 怎么解决
1. 无上限（内部用**红黑树**管理 fd，不是堆！）
2. 只在 epoll_ctl(add/mod/del) 时拷贝一次
3. 直接返回就绪事件列表

## 三个 API
`epoll_create1` → `epoll_ctl` → `epoll_wait`

## 关联
[[LT与ET]]、[[EventLoop]]、[[Poller]]
