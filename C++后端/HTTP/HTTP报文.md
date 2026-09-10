# HTTP 报文

一句话：请求/响应都是"起始行 + 头部 + 空行(\r\n\r\n) + 正文"的字节流。

## 请求报文
```
GET /index.html HTTP/1.1\r\n
Host: localhost:8080\r\n
\r\n
```

## 响应报文
```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 13\r\n
\r\n
<html>...</html>
```

## 关键
- 每行以 `\r\n` 结尾
- 头部和正文用空行 `\r\n\r\n` 分隔

## 关联
[[消息边界]]、[[keep-alive]]、[[WebServer]]
