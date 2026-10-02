# CMake

一句话：构建系统的"生成器"——你写描述（CMakeLists.txt），它生成 Makefile，make 再调 g++ 干活。

## 关系链
```
CMakeLists.txt ──cmake 配置──► build/Makefile ──make──► g++ 编译链接 ──► 可执行文件
```
- CMake **自己不编译**，只把"意图"翻译成平台相关的构建文件（Linux→Makefile，Windows→VS 工程）→ 跨平台
- 真正干活的还是 g++：证据在 `build/CMakeFiles/<目标>.dir/` 里的 `flags.make`（编译参数）和 `link.txt`（链接命令）——就是 CMake 替你生成的 g++ 命令

## 两个阶段（别搞混）
- **配置**：`cmake -S . -B build`（改了 CMakeLists.txt 必须重跑；`-D` 可设变量，如 `-DCMAKE_BUILD_TYPE=Release`）
- **构建**：`cmake --build build -j`（每次改代码后跑这个）
- 产物全在 `build/`；玄学报错就 `rm -rf build` 重来（build 可随时丢弃重建，源码不受影响）

## 核心：一切皆 target
| 目的 | 命令 |
|---|---|
| 可执行文件 | `add_executable(名 源文件...)` |
| 库（header-only 用 INTERFACE） | `add_library(名 INTERFACE)` |
| 头文件路径（= -I） | `target_include_directories(目标 关键字 include)` |
| 链接依赖（= -lpthread） | `target_link_libraries(目标 关键字 依赖...)` |
| 找系统库 | `find_package(Threads REQUIRED)` → 用 `Threads::Threads` |
- 铁律：`target_xxx` 必须写在对应 `add_xxx` **之后**（目标得先存在）；一个 target 只能定义一次

## PRIVATE / PUBLIC / INTERFACE（面试点）
一句话："这条属性，是我自己要用，还是给用我的人用？"

| 关键字 | 含义 |
|---|---|
| `PRIVATE` | 只有我自己用 |
| `INTERFACE` | 我自己不用，只告诉"用我的人" |
| `PUBLIC` | 两者都算 |

- 本项目：`add_library(reactor_core INTERFACE)` + `target_include_directories(reactor_core INTERFACE include)`
  —— 框架是 header-only（没有 .cpp 要编），这个库的全部价值就是**把 include 路径传给使用者**，所以用 INTERFACE
- 面试延伸：PUBLIC 会"传染"（A 链 PUBLIC B，C 链 A → C 自动拿到 B 的属性）；PRIVATE 不会

## 构建类型（换挡）
- 默认"空挡"（无优化）；`Debug` = `-g`（能调试）；`Release` = `-O3 -DNDEBUG`（跑得快，压测/发布用）
- 用法：`cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`

## 面试高频题
**Q: 为什么需要构建系统？**
A: 手敲 g++ 参数靠人记、无依赖图（改个头文件不知道该重编谁）、产物污染源码目录、不能跨平台；构建系统自动管依赖和增量编译。

**Q: CMake 和 Makefile 什么关系？**
A: 生成关系——CMake 读 CMakeLists.txt 生成 Makefile，make 再执行 g++；同一份描述换平台生成不同构建文件。

**Q: INTERFACE 库是什么？**
A: 自己没有源文件要编译（典型：header-only），只负责把"使用要求"（头文件路径、依赖）传递给链接它的目标。

## 关联
[[WebServer]]、[[GTest]]
