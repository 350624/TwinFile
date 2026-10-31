# 可选外部工具与完整测试依赖

Git 源码仓库不存放 EXE、DLL、JAR、JRE 或手机 APK；完整工具随 Releases 安装包部署，实际版本与来源证据见 [SOURCES.md](../licenses/SOURCES.md)，逐文件校验见运行目录 `release-components.json`。核心 C++ 应用可以先编译；下列工具为对应功能和完整回归/打包所需，源码构建者需自行准备。

## 目录布局

在本仓库 `tools` 下自行准备，或放在外部目录并给 `build.ps1 -ToolRoot` / CMake `TWINFILE_TOOL_ROOT`：

```text
TwinFileTools/
  adb/
    adb.exe
    AdbWinApi.dll
    AdbWinUsbApi.dll
    fastboot.exe
  apktool.jar
  arsclib.jar
  ecj.jar
  apksigner.jar
  runtime/
    bin/java.exe
    bin/keytool.exe
    ... 完整 Java 发行版的其余内容、release、NOTICE、legal ...
  hfx/
    HybridFileXfer-phone.apk
```

不要只复制 Java 的两个 EXE。`tools` 中除本说明外的文件默认被 Git 忽略，避免误传工具和密钥。运行包将这些目录放到 `TwinFile.exe` 旁的 `tools`；缺失时程序会报错，部分可执行文件还会查 PATH。

## 来源、用途与注意事项

| 工具 | 用途 | 官方来源 |
| --- | --- | --- |
| Android SDK Platform-Tools for Windows | 手机检测、ADB 服务、安装、端口转发；Fastboot 供可信脚本使用 | [Android Platform-Tools](https://developer.android.com/tools/releases/platform-tools) |
| Apktool | APK 反编译/回编译、必要时 aapt2 回退 | [Apktool 官方](https://apktool.org/) / [源码与 Releases](https://github.com/iBotPeaches/Apktool) |
| Eclipse ECJ 3.33.0（仅构建时） | 编译 `assets/DexBridge.java` 为嵌入程序的 Java 桥；不随运行包部署 | [Eclipse JDT](https://github.com/eclipse-jdt/eclipse.jdt.core) / [Maven Central](https://repo.maven.apache.org/maven2/org/eclipse/jdt/ecj/3.33.0/ecj-3.33.0.jar) |
| Android SDK Build-Tools 中的 apksigner.jar | APK 签名与验证 | [apksigner 文档](https://developer.android.com/tools/apksigner)，JAR 通常在 Build-Tools 的 `lib` 中 |
| Eclipse Temurin Java 17 | 运行 Java 工具，keytool 创建本地调试密钥 | [Adoptium](https://adoptium.net/temurin/releases/?version=17) |
| HybridFileXfer Android v3.0.0 | 可选 HFX 手机端及部分真实 APK 测试样本 | [上游](https://github.com/weixiansen574/HybridFileXfer)，源码提交与修改见下文 |

只从可信上游取得，核对版本、哈希、许可证及附属依赖，再自行测试。此处没有编造一套“与历史二进制逐字节相同”的下载链接或哈希；用新版工具构建可能出现兼容差异。将包重新分发时，需保留每个工具实际版本的通知和对应源码说明。

HFX 上游提交为 `c4981456aa240212b282929a69658189aa89574c`，源码 ZIP 与 GPL 在 `licenses`。历史手机端对 `.ClientActivity` 做了 `android:exported="true"` 修改并重新签名，**不是原封不动的官方 APK**。按 [HFX-MODIFICATIONS.md](../licenses/HFX-MODIFICATIONS.md) 在受支持的 Android/Gradle 环境构建并使用自己的密钥；历史签名私钥不在仓库，也不应索取或上传。桌面使用明文、无认证协议，仅限可信网络。

## 测试与发布范围

AXML / ARSC 编辑另需执行 `pwsh -File tools/prepare-android-editor.ps1`，准备固定 SHA256 的 ARSCLib 1.4.0；完整测试与打包重新校验并部署 `arsclib.jar`。桥接源码为 `assets/AndroidBinaryBridge.java`，来源、源码与许可见 `licenses/ANDROID-EDITOR.md`。

完整 PPTX 预览另需执行 `pwsh -File tools/prepare-slides.ps1`，将 Apache POI 5.5.1 和固定依赖下载到 `tools/slides`；脚本只从 Maven Central 获取，并核对仓库锁定的 SHA256。完整构建重新验证这些文件，桥接源码为 `assets/SlideBridge.java`，复用 Java 17 和 ECJ。依赖版本、哈希和源码坐标见 `tools/slides-dependencies.json`；运行包保留原 JAR 内 LICENSE/NOTICE 及 `licenses/SLIDES.md`。

- 没有工具时使用 `build.ps1 -SkipTests` 编译，或直接 CMake `-DBUILD_TESTING=OFF`。
- 完整测试需要上述布局，其中若干测试检查历史 HFX 样本具体结构；仅放任意同名 APK 不能保证全通过。自建版本的差异应单独说明，不要为得到“全通过”而删除断言。
- `build.ps1 -Package` 要求完整工具目录，即使同时指定 `-SkipTests` 也会预检。
- Git 源码与 Release 安装包分开管理；不能据本项目 GPL 认定第三方二进制也可随意发布，需保留实际通知和对应源码说明。尚未完成所有继承工具的逐字节重建核验。
