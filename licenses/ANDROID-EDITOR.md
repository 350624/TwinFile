# Android 二进制编辑器依赖

内置 AXML / ARSC 编辑器使用 REAndroid ARSCLib 1.4.0，Apache License 2.0。保留 JAR 内原始许可和归属信息；Apache 2.0 许可全文随本目录 `Apache-2.0.txt` 提供。TwinFile 的 Java 桥源码位于 `assets/AndroidBinaryBridge.java`。

- 上游：<https://github.com/REAndroid/ARSCLib>
- Maven 构件、POM 与对应源码：<https://repo.maven.apache.org/maven2/io/github/reandroid/ARSCLib/1.4.0/>
- 运行 JAR SHA256：`0cc95b160ba4c731a5cadb455f32b79f4e64b4160bc5ec3e5183ea3676b385ce`
- 准备脚本：`tools/prepare-android-editor.ps1`；完整构建重新核对 SHA256。

组件离线运行，包含上游 Android 框架资源，用于识别标准属性和枚举；不安装应用、不上传 APK、不自动签名。不能据此保证任意加固、混淆或损坏资源均可回编译。
