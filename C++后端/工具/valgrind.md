# valgrind

一句话：检测内存问题的工具（泄漏、越界、非法访问）。

## 看报告三步
1. 先看 `ERROR SUMMARY`（非法内存访问，最严重）
2. 再看 `LEAK SUMMARY` 的 `definitely lost`（真泄漏）
3. 只有非 0 才翻调用栈定位

## 分类
- `definitely lost`：真泄漏（没指针指着了）
- `still reachable`：程序结束时还有指针指着，通常不是 bug

## 用法
```bash
valgrind --leak-check=full --show-leak-kinds=all ./program
```

## 关联
[[strace]]、[[ab压测]]
