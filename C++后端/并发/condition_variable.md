# condition_variable

一句话：让线程"等条件成立且不烧 CPU"——等待时释放锁，被唤醒时重新拿锁。

## 核心
`cv.wait(lock, pred)` 等价于：
```cpp
while (!pred()) { cv.wait(lock); }   // 注意是 !pred
```

## 为什么必须 unique_lock
wait 需要中途 unlock 再 lock，`lock_guard` 做不到。

## 三个作用
1. 释放锁 2. 进入睡眠 3. 被唤醒后重新拿锁

## 关联
[[mutex与死锁]]、[[Buffer]]（生产者消费者模式）
