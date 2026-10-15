# Windows 发布包检查

以下是首次 1.0.0 的历史检查记录，其中的 EXE 哈希仅适用于旧版。1.0.1 的修复与发布验证见 [PROXY-FIX.md](PROXY-FIX.md)。

检查日期：2026-10-02。应用：LearningC Studio 1.0.0，Windows x64 Release，Qt 6.8.3。

## 实际结果

1. `package.ps1` 执行完整 Release 构建、四组 CTest、`windeployqt` 与 MSVC CRT 部署，均成功。
2. 从 `dist/LearningCStudio` 启动，启动环境的 PATH 仅保留 Windows/System32 与 Windows，清除 Qt 插件路径和缩放变量。
3. 读取该测试进程实际加载模块，Qt6Core、Qt6Gui、Qt6Widgets、Qt6Network、qwindows 和 MSVC CRT 全部来自发布目录。明细见 [package-modules.json](test-results/package-modules.json)。该隐藏检查进程由测试工具结束。
4. 使用同一环境运行 `learningc_studio.exe --isolated --screenshot <文件>`，成功生成 [发布包界面截图](screenshots/05-packaged.png)，进程自动正常退出，退出码 0。
5. 构建目录与发布目录中的 EXE SHA-256 一致：

```text
045504E9D3F24647882D47DA635D9ECA40EA74B56BA7CE8E11C3CB2A9483D403
```

上述检查使用 `--isolated`，不会读取或写入用户的服务器配置。普通启动会保存服务器配置；请求历史仅保留在内存中。

## 包含内容

- `learningc_studio.exe` 与 Qt Core/Gui/Widgets/Network DLL。
- `platforms/qwindows.dll`、图像格式、网络信息、Windows Schannel TLS 等插件。
- app-local MSVC x64 CRT DLL，另保留部署工具附带的 `vc_redist.x64.exe`。已在本机通过发布目录中的 DLL 启动，无需为本次检查运行安装器。
- 中文 README、架构与面试阅读提纲、操作演示、测试记录、截图及许可说明。

部署工具提示缺少翻译索引与可选 `dxcompiler/dxil`：应用中文文案直接编入程序，绘图采用 Qt Widgets/QPainter；本次不依赖 Qt 翻译目录或 DirectX 12 着色器编译器，发布目录启动已验证。

## 适用边界

检查在当前开发机的独立发布目录完成，证明启动时使用发布包自身的 Qt/MSVC 文件；尚未使用全新 Windows 虚拟机验证。真实 Linux 后端、跨显示器动态 DPI 与长时间运行仍按 [测试报告](TESTING.md) 标记范围处理。
