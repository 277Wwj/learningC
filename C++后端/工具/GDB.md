# GDB

一句话：让程序"慢动作重放 + 暂停"的调试器——崩溃时看现场，运行时蹲点。

## 段错误定位四步（面试答案）
1. 用 `-g` 编译（Debug 档）→ `gdb -q ./程序` → `run`（或 `gdb -p PID` attach 已在跑的进程）
2. 崩溃时 GDB 直接停在出错行，连参数值都给你（如 `handleRequest (c=0x0)`）
3. `bt` 看调用栈：谁调用了谁（从下往上读是调用方向）
4. `frame N` 切帧 + `print 变量` 验证：空指针？越界？输入值是多少？

## 常用命令
| 命令 | 作用 |
|---|---|
| `gdb -q ./程序` | 加载（不加 -q 有横幅分页，按 `q` 跳过） |
| `run`(r) / `continue`(c) | 运行 / 继续 |
| `bt` | **调用栈**（最重要） |
| `frame N`(f N) | 切到第 N 层栈帧 |
| `print x`(p x) | 看变量、指针的值 |
| `break 位置`(b) | 断点（函数名或 `文件:行号`） |
| `next`(n) / `step`(s) | 单步 / 进入函数单步 |
| `info threads` / `thread N` | 看所有线程 / 切换线程 |
| `detach` / `quit` | 脱离（让进程恢复运行）/ 退出 |

## 两大姿势
- **本地**：`gdb ./prog` + `run`（gdb 是父进程，畅通无阻）
- **线上 / 进程已在跑**：`gdb -p $(pgrep -f 程序名)` attach
  - Ubuntu 默认 `ptrace_scope=1` 会拦"非父子"attach → `sudo sysctl kernel.yama.ptrace_scope=0`（临时，重启失效）或改 `/etc/sysctl.d/10-ptrace.conf`（永久）
  - attach 会**暂停**进程，看完记得 `detach` 恢复

## 调服务器：暂停键 / 播放键
- `Ctrl+C` = ⏸ 暂停（还你命令行）；`continue` = ▶ 恢复
- `while(true)` 的服务器 `continue` 后不会"结束"，这是正常的（它在正常值班）
- 看线程现场：`info threads` → `thread 2` → `bt`（正常服务器线程都停在 `epoll_wait`）

## 踩过的坑
- 复合命令先拆开跑：`pgrep -f 程序名` 没输出 = 进程没在跑（否则 `gdb -p $(...)` 报"-p 需要一个参数"）
- **报错行 ≠ 出错行**（比如名字拼错，报在下一行）
- 输出分页卡住：按 `q`；嫌烦用 `gdb -q` 启动或 `set pagination off`

## 面试高频题
**Q: 段错误怎么定位？** 见上方四步；线上没现场就 core dump 事后分析。
**Q: 服务器卡死怎么查？** `gdb -p` attach → `info threads` + 逐线程 `bt`：卡在 `epoll_wait` 正常，卡在锁/系统调用就要怀疑死锁。

## 关联
[[WebServer]]、[[strace]]、[[valgrind]]
