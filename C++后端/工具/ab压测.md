# ab 压测（ApacheBench）

一句话：测 HTTP 服务器 QPS 的工具。

## 用法
```bash
ab -n 100000 -c 100 http://127.0.0.1:8080/     # 短连接
ab -k -n 100000 -c 100 http://127.0.0.1:8080/  # 长连接
```

## 看什么
- `Requests per second` = QPS
- `Failed requests` 应该为 0

## 我的实测
[[WebServer]]：短连接 7336 QPS，长连接 36927 QPS。

## 关联
[[keep-alive]]、[[WebServer]]
