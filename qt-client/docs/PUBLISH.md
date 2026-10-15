# 1.0.2 源码上传与验证记录

日期：2026-10-15。

## 远端基线与交付范围

以 `277Wwj/learningC` 的 `main` 提交 `f82a2b7c7ac46d7e91a5c7f925d55907a807c0ed` 为基础，在独立克隆目录合入 Qt 客户端。保留远端的 Linux 服务端、头文件、测试和学习笔记，不用旧本地快照覆盖它们。

本次提交包含：

- 独立 `qt-client` 工程：HTTP 调试、轻量压测、LCH1 TCP 帧调试。
- Windows 构建与部署脚本、Qt Test 自动化测试、架构和操作文档、历史截图与许可文本。
- 根 README 的模块导航、构建入口、各 HTTP 版本路由说明，以及正确的 CTest 构建目录。
- 按最新聊天服务更新 Login/Welcome/Users、群聊/私聊和 Bye 的说明与 TCP 页面提示；客户端版本更新为 1.0.2。
- `.gitignore` 排除 Qt 构建目录、SDK 和发布目录。

EXE、DLL、ZIP、构建缓存和与项目无关的个人文件不提交到源码仓库。运行包可通过 `qt-client/scripts/package.ps1` 在本机生成；此记录不表示 GitHub 已提供 Release 附件。

## 本次实际验证

环境：Windows x64、Qt 6.8.3、MSVC 19.41（Visual Studio 2022）、CMake 3.29.5、Release。

在独立克隆目录执行：

```powershell
.\qt-client\scripts\build.ps1 -QtPath 'F:\codexproject\.qt\6.8.3\msvc2022_64'
```

该路径是本机 SDK 路径，其他机器请替换。配置、完整编译和四组 CTest 均成功，CTest 用时约 16.04 秒。Qt Test 共 47 项通过（含每组初始化/清理），39 个具体用例，0 失败、0 跳过。测试使用本机受控服务；测试明细保存于构建目录的 `test_*-results.txt`，本次副本位于 [test-results/1.0.2](test-results/1.0.2/)。

旧版截图、代理修复复现与发布包 SHA-256 仍保留在历史文档中，仅适用于对应版本。本次没有重新打包，也没有在全新 Windows 系统验证部署。

## 尚待真实环境验证

- 最新 Linux 后端的双客户端登录、Users 更新、群聊、私聊与 Bye 流程。
- 最新服务端在分段输入、EOF、超长转发载荷及慢客户端积压下的行为。
- Linux 桌面下 Qt 客户端的构建和运行。

上述项目不能用 Windows 客户端测试通过来代替。复现步骤见 [DEMO.md](DEMO.md)。本次只更新客户端的提示与说明，没有修复或修改后端实现。
