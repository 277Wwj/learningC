# Buffer

一句话：双指针缓冲区，把 recv 到的字节攒起来，解决 [[粘包与半包]]。

## 设计
`readIndex_`（可读数据起始）和 `writeIndex_`（可写空间起始）

## 关键方法
- `readFd`：循环读到 EAGAIN，返回 -1 表示对端关闭
- `peek`：不拷贝，直接看
- `retrieve(n)`：消费 n 字节（keep-alive 时只消费一个请求，不清空）

## 关联
[[粘包与半包]]、[[消息边界]]、[[keep-alive]]
