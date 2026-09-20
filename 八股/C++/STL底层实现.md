# STL 底层实现

一句话：vector 连续数组按倍数扩容，map 红黑树，unordered_map 哈希表。

## vector
- 连续内存，`size` 是元素数，`capacity` 是容量
- 满了按 2 倍（GCC）/1.5 倍（MSVC）扩容：分配新内存 → 搬旧元素 → 释放旧内存
- 按倍数扩容的均摊复杂度 O(1)（每次固定 +1 是 O(n²)）
- 扩容导致迭代器失效；`reserve(n)` 预留空间避免反复扩容

## map（红黑树）
- 有序（按 key 排序），查找/插入/删除 O(log n)
- 用红黑树而非 AVL：插入删除旋转更少

## unordered_map（哈希表）
- 无序，平均 O(1)，最坏 O(n)
- 哈希冲突用拉链法；元素多触发 rehash

## 面试高频题
**Q: vector 扩容会迭代器失效吗？**
A: 会。扩容释放旧内存，旧迭代器指向野内存。

**Q: map 和 unordered_map 怎么选？**
A: 需要按 key 有序遍历用 map；只快速查找、不关心顺序用 unordered_map。

**Q: reserve 和 resize 区别？**
A: reserve 只改 capacity；resize 改 size 且会构造/析构元素。
