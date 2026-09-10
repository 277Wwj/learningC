# mutex 与死锁

一句话：mutex 管互斥；死锁 = 两个线程互相等对方手里的锁。

## 死锁四条件
互斥、持有并等待、不可剥夺、循环等待

## 经典案例（我的代码）
Threadpool 析构时在锁内 `join`：worker 被唤醒后要重新拿锁才能退出，
但主线程持锁等 join → 双向等待死锁。
**解决：join 移出锁外。**

## 关联
[[condition_variable]]、[[WebServer]]
