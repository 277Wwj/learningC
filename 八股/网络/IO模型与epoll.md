# IO 模型与 epoll

一句话：epoll 是"一个线程同时监视多个 fd"的最优 IO 多路复用方案。

## IO 模型演进
| 模型 | 特点 |
|------|------|
| 阻塞 IO | 没数据卡住 |
| 非阻塞 IO | 没数据返回 EAGAIN |
| select | 最多 1024 fd、全量拷贝、O(n) 扫描 |
| poll | 去掉 1024 上限，仍 O(n) |
| epoll | 红黑树 + 就绪链表，只返回就绪事件 |

## epoll 三个 API
- `epoll_create1`：创建实例
- `epoll_ctl`：增删改 fd（EPOLL_CTL_ADD/MOD/DEL）
- `epoll_wait`：阻塞等就绪事件

## LT vs ET
| | LT 水平触发 | ET 边缘触发 |
|---|------------|------------|
| 触发 | 有数据就一直通知 | 只在变化瞬间通知一次 |
| 要求 | 无 | 非阻塞 + 循环读到 EAGAIN |

## 面试高频题
**Q: epoll 为什么比 select 快？**
A: ① 无 fd 上限 ② 只在 epoll_ctl 时拷贝一次（select 每次全量拷贝）③ 直接返回就绪列表（select 要遍历全部）。

**Q: ET 为什么必须配非阻塞？**
A: ET 要求循环读直到 EAGAIN，若阻塞，最后一次读会卡住。

**Q: epoll 底层数据结构？**
A: 红黑树管理 fd，就绪链表存就绪事件。
