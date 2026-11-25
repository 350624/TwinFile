# HFX 随附组件
来自 TwinFile 原版 tools/adb、tools/hfx 和 licenses。运行时按需将 adb 及两枚伴随 DLL 解包至当前用户的 LOCALAPPDATA/TwinFileRust/hfx-v300-adb37.0.1；只有用户明确同意后，才准备并安装配套 APK。不会把额外 EXE 放到桌面或发布目录。

- ADB 1.0.41, platform-tools 37.0.1-15733141；来源 https://android.googlesource.com/platform/packages/modules/adb/ 和 Android SDK platform-tools。保留 Apache-2.0.txt；未修改二进制。未使用 fastboot。
- Android 客户端 HybridFileXfer v3.0.0：完整上游源码见 sources，许可证和原版对 manifest/签名的改动见 HFX-GPL-3.0.txt 和 HFX-MODIFICATIONS.md。
- 程序不主动安装手机端、不替用户授权、不修改防火墙。关闭连接时清理本会话创建的 USB reverse 端口；不会关闭其它程序的 ADB 服务。