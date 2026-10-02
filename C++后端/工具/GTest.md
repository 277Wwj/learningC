# GTest（单元测试）

一句话：给代码写"自动检查"——喂输入、验输出，机器帮你确认"没改坏"。

## 为什么（面试话术）
- **防回归**：重构/改代码后跑一遍，就知道有没有改坏东西
- 逼你把接口设计干净（难测的代码通常是设计有问题）
- 加分项：核心模块 [[Buffer]] 有单元测试，覆盖正常流程 + 边界

## CMake 集成
```cmake
enable_testing()
find_package(GTest REQUIRED)

add_executable(buffer_test tests/buffer_test.cpp tests/eof.cpp)
target_link_libraries(buffer_test PRIVATE reactor_core GTest::gtest GTest::gtest_main)

add_test(NAME buffer_test COMMAND buffer_test)
```
- 测试程序**不用写 main**（`gtest_main` 提供）；一个测试程序可包含多个测试文件
- 跑法：`./build/buffer_test` 或 `cd build && ctest --output-on-failure`

## 写法：三段式
```cpp
TEST(组名, 用例名) {
    // ① 准备（造数据/造连接） ② 操作（调用被测函数） ③ 断言（验证结果）
    EXPECT_EQ(实际, 期望);   // 失败记录并继续；ASSERT_ 开头失败则停止本用例
}
```
- 断言对象 = **"状态"或"有返回值的东西"**（`void` 函数只能靠状态断言）
- 测 socket 用 `socketpair` 造一对假连接，读端设非阻塞（否则 readFd 会卡住等数据）

## 写测试发现的语义（意外收获）
- `readFd` 遇到对端关闭（EOF）时：**即使本轮读到过数据也返回 -1**；数据仍在 buffer 里，要用 `readableBytes()` 判断
- 边界用例"取多了"：`retrieve(1000)` 但只有 5 可读 → 安全清空（可读变 0），不崩

## 面试高频题
**Q: 你怎么保证代码质量？**
A: 核心模块写单元测试——正常流程 + 边界（取多了）+ 异常（对端关闭）；重构后跑回归测试；出问题用 [[GDB]] 定位。

## 关联
[[Buffer]]、[[CMake]]、[[WebServer]]
