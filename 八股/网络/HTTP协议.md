# HTTP 协议

一句话：HTTP 是"起始行 + 头部 + 空行(\r\n\r\n) + 正文"的字节流协议。

## 请求报文
```
GET /index.html HTTP/1.1\r\n
Host: localhost:8080\r\n
\r\n
```
- 请求行：方法 路径 版本
- 头部：键: 值
- 空行：头部结束

## 响应报文
```
HTTP/1.1 200 OK\r\n
Content-Length: 13\r\n
\r\n
<html>...</html>
```

## 方法
- GET：获取资源（读）
- POST：提交/上传数据（写），数据在请求体

## keep-alive（长连接）
- HTTP/1.1 默认长连接，一个连接处理多个请求
- HTTP/1.0 默认短连接，响应后关闭
- 好处：省握手/挥手开销

## 面试高频题
**Q: HTTP 怎么解决消息边界？**
A: 分隔符（\r\n\r\n 找头部结束）+ Content-Length（定正文长度）双管齐下。

**Q: 状态码？**
A: 200 成功、301/302 重定向、404 未找到、500 服务器错误。

**Q: HTTP 和 HTTPS 区别？**
A: HTTPS 加了 TLS 加密层，默认 443 端口，防止窃听篡改。
