# 发布依赖与源码获取

TwinFile 源码在 <https://github.com/350624/TwinFile>；每个正式 Release 附对应提交的完整项目源码 ZIP，包含实际构建的 vendor 源码和 HFX 上游源码/修改说明。以下记录 3.1.2 发布环境，不声称由这些网址下载的任意文件与继承的历史工具逐字节相同。实际部署文件哈希见安装目录 `release-components.json`。

| 组件 | 本次版本证据 | 上游源码 / 获取位置 |
| --- | --- | --- |
| Qt | 6.10.3，动态 DLL 文件版本及官方 SDK SBOM | <https://download.qt.io/official_releases/qt/6.10/6.10.3/single/>；模块源码也在 <https://code.qt.io/cgit/qt/> 的 v6.10.3 标签 |
| FFmpeg | 7.1.3，运行库 `av_version_info`，与 Qt Multimedia SBOM 一致 | <https://ffmpeg.org/releases/ffmpeg-7.1.3.tar.xz>；<https://github.com/FFmpeg/FFmpeg/tree/n7.1.3> |
| GCC / MinGW | GCC 13.1 工具链，运行库动态部署 | <https://ftp.gnu.org/gnu/gcc/gcc-13.1.0/>；<https://download.qt.io/development_releases/prebuilt/mingw/>；<https://www.mingw-w64.org/> |
| ADB / Fastboot | 37.0.1-15733141，adb 1.0.41 | <https://developer.android.com/tools/releases/platform-tools>；<https://android.googlesource.com/platform/packages/modules/adb/>；<https://android.googlesource.com/platform/system/core/> |
| Apktool | JAR 自报 3.0.3 | <https://github.com/iBotPeaches/Apktool/tree/v3.0.3> |
| apksigner | JAR 自报 0.9；此号不能唯一确定 Android Build-Tools 发行版 | <https://android.googlesource.com/platform/tools/apksig/>；<https://developer.android.com/tools/apksigner> |
| Eclipse Temurin | 17.0.20.1+1，JRE 自带 release 文件 | <https://github.com/adoptium/jdk17u>，源码提交 79597447bd94；构建仓库 <https://github.com/adoptium/temurin-build> 提交 e6ba7dec3d07654074559310376a3ae89da5f4ac |
| HybridFileXfer | Android v3.0.0，历史修改及重签名 | 本目录 `sources/HybridFileXfer-v3.0.0-source.zip`、`HFX-MODIFICATIONS.md`；上游提交 c4981456aa240212b282929a69658189aa89574c |

FFmpeg 运行库自报 `LGPL version 2.1 or later`，实际配置为：

```text
--prefix=/c/FFmpeg-n7.1.3/build/mingw/installed --disable-programs --disable-doc --disable-debug --enable-network --disable-lzma --enable-pic --disable-vulkan --disable-v4l2-m2m --disable-decoder=truemotion1 --enable-shared --disable-static
```

Qt SDK 的模块 CycloneDX SBOM 随运行包保存在 `licenses/qt-sbom`，包含第三方归属及许可文本；MinGW 实际工具链通知在 `licenses/mingw`，Java 通知与模块法律文件保存在 `tools/runtime/NOTICE`、`legal`。Apktool/apksigner 的 JAR 原始元数据与通知保留在归档内。各组件仍按自身许可证分发，不因 TwinFile GPL 声明改变。

可以修改并重新链接本项目动态依赖；不要混用不兼容 Qt ABI 或运行库。尚未完成每个历史工具与上游重建结果逐字节复现，也未作完整法律审计。如发现源码获取失效、通知遗漏或许可问题，请通过仓库 Issues / `SECURITY.md` 中的渠道反馈，发布者应补齐或停止受影响组件分发。
