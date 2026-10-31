# 第三方组件、来源与发布边界

TwinFile 原创部分按 GPL-3.0-only 发布，见 `LICENSE`、`NOTICE.md`。本文件不改变第三方原作者许可，也不是二进制再发行合规审计结论。Git 仓库仅包含原生源码、资源、文档及保留通知的 vendor / HFX 上游源码，不包含 Qt/FFmpeg/Java/Android 工具二进制、个人配置或历史安装包。以下涉及“包内”的内容描述完整本地构建的部署行为，不表示仓库已提供这些文件。

- **Qt 6.10.3**：动态链接 Core/Gui/Widgets/Concurrent/Network/Xml/Svg/Multimedia/MultimediaWidgets，测试另用 Test。各模块以实际许可为准，通常 LGPLv3/GPLv3 或商业许可。标准文本在 `licenses/LGPL-3.0-only.txt`、`GPL-3.0-only.txt`；<https://www.qt.io/licensing/>、<https://download.qt.io/official_releases/qt/6.10/6.10.3/>。

- **Apache POI 5.5.1 / XMLBeans / Commons / Log4j API / CurvesAPI / SparseBitSet**：PPTX 只读查看器复用成熟的 OOXML 渲染组件，通过 Java 17 子进程离线渲染。完整版本、来源和许可见 `licenses/SLIDES.md`，固定依赖哈希见 `tools/slides-dependencies.json`；包内保留未经修改的 JAR 及其中 LICENSE/NOTICE。
- **FFmpeg 7.1.3 动态库**：随 Qt 部署的解码后端，运行库 `av_version_info` 与 Qt Multimedia SBOM 一致，自报 LGPL-2.1-or-later；实际配置不启用 GPL / nonfree。配置、源码和版本证据见 `licenses/SOURCES.md`；<https://ffmpeg.org/legal.html>。格式支持不代表获得相关专利或商标授权。
- **zlib 1.3.2 / MiniZip**：保留 `vendor/zlib` 原通知；包内 `licenses/zlib.txt`、`Info-Zip.txt`。源码 <https://zlib.net/zlib-1.3.2.tar.gz>，SHA256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`。
- **bzip2 1.0.8**：Payload BZ2。原许可 `vendor/bzip2/LICENSE`，包内 `licenses/bzip2.txt`；<https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz>，归档 SHA256 `ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269`。
- **XZ Utils 5.8.1 / liblzma**：Payload XZ。实际链接 liblzma 为 0BSD；其它发行文件各按自身许可，见 `vendor/xz/COPYING*`、包内 `licenses/XZ-0BSD.txt`；<https://github.com/tukaani-project/xz/releases/tag/v5.8.1>，归档 SHA256 `507825b599356c10dca1cd720c9d0d0c9d5400b9de300af00e4d1ea150795543`。
- **Zstandard 1.5.7**：Payload ZSTD，BSD-3-Clause 选项，保留 `vendor/zstd/LICENSE` 和包内 `licenses/Zstandard-BSD.txt`；<https://github.com/facebook/zstd/releases/tag/v1.5.7>，归档 SHA256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`。
- **libwebp 1.6.0**：静态链接解码器，用于 APK 中的 WebP 图标，不依赖额外 Qt 图片插件。保留 `vendor/webp/COPYING`、`PATENTS`、`AUTHORS`；包内 `licenses/libwebp-BSD.txt`、`libwebp-PATENTS.txt`。源码 <https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz>，归档 SHA256 `e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564`。仅构建所需解码目标，不部署编码器或命令行工具。
- **ADB / Fastboot 37.0.1-15733141**：沿用旧工程 `tools/adb`，本次通过各工具 version 命令确认，ADB 1.0.41；上游 <https://android.googlesource.com/platform/packages/modules/adb/>、<https://android.googlesource.com/platform/system/core/>，保留 Apache/BSD 通知和 `licenses/Apache-2.0.txt`；未完成与上游重建的逐字节复现。
- **apktool 3.0.3 / apksigner 自报 0.9**：沿用旧 JAR，未修改；上游 <https://github.com/iBotPeaches/Apktool>、<https://android.googlesource.com/platform/tools/apksig/>，Apache-2.0 及 JAR 内依赖许可适用；apksigner 自报号不能唯一确定 Build-Tools 发行版。
- **smali / baksmali / dexlib2**：DEX 编辑器复用 Apktool 内的 3.0.9-dev 开放组件，归属与通知保留于原 JAR；上游 <https://github.com/google/smali>。本项目原创桥接源码为 `assets/DexBridge.java`，编译资源嵌入应用，不修改上游组件。**Eclipse ECJ 3.33.0**（EPL-2.0）仅用于构建桥接资源，不部署到运行包；来源和 SHA256 见 `licenses/SOURCES.md`。
- **Eclipse Temurin Java 17**：原 `tools/runtime/release`、`NOTICE`、`legal` 完整保留；Java 模块通常 GPLv2 + Classpath Exception，附属库遵循自身许可。精确来源按 `release` 中 Adoptium 提交；<https://github.com/adoptium/jdk17u>、<https://adoptium.net/>。
- **HybridFileXfer Android v3.0.0**：GPL-3.0，含完整上游源码 ZIP、许可证、修改和重建说明。上游 <https://github.com/weixiansen574/HybridFileXfer>，提交 `c4981456aa240212b282929a69658189aa89574c`。**不是未修改的官方 APK**：历史包导出 ClientActivity 并重新签名，见 `licenses/HFX-MODIFICATIONS.md`；不声称持有历史私钥。无认证/明文协议仅适用于可信手机和网络。
- **MinGW GCC 13.1 运行库**：libgcc/libstdc++/winpthread 适用各自许可和 GCC Runtime Library Exception；<https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html>、<https://www.mingw-w64.org/>。构建脚本从实际编译器复制 gcc / mingw-w64 / winpthreads 通知到包内 `licenses/mingw`，来源见 `licenses/SOURCES.md`。

`vendor/` 保留实际构建所需的原生源码和上游通知，移除了不参与本项目构建的 Python 辅助文件，没有重新许可原生库。电脑业务不需要旧 Python HFX 主机代码或原电脑端 HFX JAR。

项目原权利归属不因重写语言而改变，GPL 声明仅覆盖项目有权许可的原创部分。发布时仍需维护 HFX、Qt/FFmpeg/Java/运行库对应源码义务及资源权利。本次 Release 从公开源码重新打包，附依赖通知、Qt SDK SBOM、实际文件 SHA256 与 `licenses/SOURCES.md`，不是仅上传旧安装器。Git 不存放工具二进制，构建者自行准备见 `tools/README.md`。版本确认不等于完整法律审计，也不声称完成所有历史工具的可复现构建。
