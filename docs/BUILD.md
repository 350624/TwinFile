# 从源码构建

[返回首页](../README.md) · [外部工具](../tools/README.md) · [第三方许可](../THIRD_PARTY.md)

## 工具链

已验证组合：Windows x64、Qt **6.10.3 MinGW 64-bit**、MinGW GCC **13.1**、CMake **3.24+**、**PowerShell 7**。Qt 需要 Core、Gui、Widgets、Concurrent、Network、Multimedia、MultimediaWidgets、Svg、Xml、Test。准备安装器另需 Inno Setup 6。请在 PowerShell 7 中执行脚本，旧 Windows PowerShell 5.1 的编码及 .NET API 不保证兼容。当前脚本和 Windows API 面向 MinGW，未验证 MSVC、ARM64 或跨平台构建。

```powershell
git clone https://github.com/350624/TwinFile.git
cd TwinFile
```

仓库已含所需 C 压缩/解码源码，不需要另外用包管理器安装 zlib、bzip2、liblzma、zstd、libwebp。**外部 Android/Java 工具没有放进 Git**。不要下载不明第三方整合包，也不要将自己的签名密钥提交到仓库。

## A. 先编译桌面程序（没有 Android 工具时）

```powershell
.\build.ps1 -QtRoot 'C:\Qt\6.10.3\mingw_64' `
  -CompilerRoot 'C:\Qt\Tools\mingw1310_64' -SkipTests
```

CMake 必须在 PATH，或传入 `-CMake 'C:\Program Files\CMake\bin\cmake.exe'`。输出 `build\bin\TwinFile.exe`；开发运行时需 Qt 和 MinGW 的 `bin` 在 PATH。这个 EXE 还不是可独立搬走的完整便携包。缺失工具不妨碍核心编译，但 ADB、APK 回编译/签名、HFX 等功能不可完整使用；跳过测试不是测试通过。

若 PowerShell 的执行策略阻止脚本，请先阅读脚本并遵守组织策略；也可直接执行下一节的 CMake 命令，无需永久降低系统执行策略。

## B. 完整构建、测试和部署

按 [tools/README.md](../tools/README.md) 准备工具根目录，包含 ADB、Java、Apktool / apksigner / ECJ 三个 JAR、HFX 测试 APK 等，再运行：

```powershell
.\build.ps1 -QtRoot 'C:\Qt\6.10.3\mingw_64' `
  -CompilerRoot 'C:\Qt\Tools\mingw1310_64' `
  -ToolRoot 'C:\TwinFileTools' -Package
```

脚本会配置 Release、编译、执行六组 QtTest，再用 `windeployqt` 部署到 `dist\TwinFile`，复制工具、SDK、示例 DLL、许可和文档。测试结果为 `build\native-test-results.txt`、`migration-test-results.txt`、`extension-test-results.txt`、`agent-test-results.txt`、`mcp-test-results.txt`、`download-test-results.txt`。直链下载测试只访问本机 HTTP 模拟服务，不下载真实固件。

DEX 编辑界面和文件操作使用 C++/Qt；构建时以 Eclipse ECJ 3.33.0 编译仓库 `assets/DexBridge.java`，嵌入两个 class 资源，运行时调用发行包既有 Java 17 / Apktool 3.0.3 内的 smali 3.0.9-dev API。ECJ 仅用于构建、不部署；没有 ECJ 的核心构建会明确警告并禁用 DEX 后端，完整测试/打包拒绝缺失依赖。不同 Apktool 内部 API 版本可能不兼容，必须实际编译和往返验证，不应随意替换 JAR。

完整测试包含实际 APK 解码/回编译/签名和历史 HFX 样本检查，**不是仅靠 Qt 就能全部通过**。与本地验证不同的工具版本或自建 APK 可能得到不同结果，不能据此声称复现了历史 262 项结果。缺失样本/工具时完整构建会预检报错。

仅构建应用、不生成测试时也可用 CMake：

```powershell
$env:Path = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.10.3\mingw_64\bin;' + $env:Path
cmake -S . -B build-core -G 'MinGW Makefiles' `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/mingw_64 `
  -DBUILD_TESTING=OFF -DTWINFILE_TOOL_ROOT=C:/TwinFileTools
cmake --build build-core --parallel 4
```

不要在同一个构建目录混用不同编译器或 Qt 版本；换工具链时用新的构建目录。`TWINFILE_TOOL_ROOT` 是明确配置的开发工具路径，不再隐式查找源码父目录；程序优先找自身目录及其 `tools`，最后也可能从 PATH 查找可执行文件。

## C. 验证与安装器

```powershell
.\verify.ps1
.\verify-ui.ps1 -QtRoot 'C:\Qt\6.10.3\mingw_64'
.\package.ps1 -SkipBuild -Compiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
```

`verify.ps1` 检查便携启动、Python 产物及源文件摘要；`verify-ui.ps1` 在 Windows 平台运行专项测试并生成截图。两者产生的本地路径、截图、JSON 和日志在 `verification`，**默认不提交 Git**。不要将自己的文件列表截图上传。

安装器输出 `releases\TwinFile-3.1-Cpp-Setup.exe` 及 `.sha256`。脚本**不执行安装器**。运行包附实际工具链通知、Qt SDK SBOM 与 `release-components.json` 文件校验清单，来源见 [THIRD_PARTY.md](../THIRD_PARTY.md)。完整发布使用 [publish.ps1 流程](RELEASING.md)，自动生成带版本的安装包名称、对应提交源码、校验值与更新日志并上传 GitHub Releases；不是上传历史旧安装器。

## D. 导出源码

```powershell
.\export-source.ps1 -Destination 'C:\Exports\TwinFile-source'
```

目标必须不存在。默认仅复制原生源码、资源、测试、vendor、licenses、SDK、文档及构建脚本；不复制 `.git`、个人配置、工具二进制、构建产物或验证记录。需本地完整备份时可显式加 `-IncludeLocalArtifacts`，再由自己检查其中是否包含敏感路径或数据，不要直接公开。
