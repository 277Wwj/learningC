# C++ 其他高频八股

## const
- 修饰变量：不可修改
- `const int* p`：指针指向的值不能改；`int* const p`：指针本身不能改
- 成员函数后加 const：不能修改成员变量，const 对象只能调 const 函数

## static
- 局部 static：只初始化一次，生命周期到程序结束
- 类 static 成员：所有对象共享，类外定义
- static 函数：无 this 指针，只能访问 static 成员

## 四种 cast
| cast | 用途 |
|------|------|
| `static_cast` | 常规转换（数值、void* 等） |
| `dynamic_cast` | 多态安全向下转型（需虚函数，失败返回 nullptr） |
| `const_cast` | 去掉 const |
| `reinterpret_cast` | 底层二进制重解释（危险） |

## new / delete vs malloc / free
- new 会调用构造函数、delete 调用析构；malloc/free 只分配/释放内存
- new 失败抛异常，malloc 失败返回 nullptr

## 内存对齐
- 结构体成员按最大对齐要求对齐，可能有填充字节
- 合理排列成员可减小结构体大小
