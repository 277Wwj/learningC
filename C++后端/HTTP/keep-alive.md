# keep-alive（长连接）

一句话：一次 TCP 连接连续处理多个请求，省掉反复握手/挥手。

## 价值（我的实测数据）
短连接 7336 QPS vs 长连接 36927 QPS（差 5 倍）

## 实现要点
- 响应头 `Connection: keep-alive`
- 处理完一个请求只 `retrieve(pos+4)`，不清空 [[Buffer]]（可能还有下一个请求）
- 副作用：僵尸连接 → 用 [[timerfd]] 超时断开

## 关联
[[HTTP报文]]、[[Buffer]]、[[粘包与半包]]、[[WebServer]]
