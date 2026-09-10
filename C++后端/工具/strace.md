# strace

一句话：跟踪系统调用，看程序和内核的每一次交互（recv/send/accept...）。

## 和 valgrind 的区别
- [[valgrind]]：用户态内存问题
- strace：系统调用层（程序 ↔ 内核）

## 用法
```bash
strace -e trace=network -o trace.txt ./program
```

## 经典应用
`recv(5, "hello\nworld\n", 1023, 0) = 12` —— 一次 recv 收到两条消息，证明 [[粘包与半包]]

## 关联
[[valgrind]]、[[粘包与半包]]
