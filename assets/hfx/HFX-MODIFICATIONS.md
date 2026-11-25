# HybridFileXfer 手机组件的来源与修改

上游：<https://github.com/weixiansen574/HybridFileXfer>，v3.0.0，对应提交 `c4981456aa240212b282929a69658189aa89574c`，GPL-3.0。完整上游源码位于同目录 `sources/HybridFileXfer-v3.0.0-source.zip`，许可证见 `HFX-GPL-3.0.txt`。

随本地构建保留的 `tools/hfx/HybridFileXfer-phone.apk` 来自旧工程，**不是未修改的官方 APK**。与官方 v3.0.0 `app-release.apk` 比较，忽略签名文件后，只有 `AndroidManifest.xml` 不同；所有 dex、资源表和其它条目逐项 SHA256 一致。本次核对未连接或安装到真实手机。

功能修改：将 `top.weixiansen574.hybridfilexfer.ClientActivity` 的 `android:exported` 从 `false` 改为 `true`，并重新签名，使用户主动启动 USB 会话时能通过 ADB 打开客户端。对应源码修改如下：

```diff
--- HybridFileXfer-Android/app/src/main/AndroidManifest.xml
+++ HybridFileXfer-Android/app/src/main/AndroidManifest.xml
@@
         <activity
             android:name=".ClientActivity"
-            android:exported="false" />
+            android:exported="true" />
```

重建：按上游 Android 工程的 Gradle/Android SDK 要求构建手机 APK，再用自己的 Android 签名密钥签名；签名不同，不能直接覆盖官方签名的安装。源码更改可产生等价功能，但不承诺与旧工程历史二进制逐字节一致，历史签名私钥没有随附。

安全差异：导出的 Activity 可被其它本机 Android 应用启动，HFX 传输协议没有认证或加密。只在可信手机和可信局域网使用，传输后断开会话。TwinFile 不自动安装手机组件、不绕过 USB 授权、不自动改防火墙。

电脑端的业务代码已使用 C++ 实现，不打包或调用旧 Python HFX 主机代码，也不需要原电脑端 `HybridFileXfer.jar`。许可证和原作者权利不因迁移而改变。此文不是第三方许可合规的法律结论；公开再发行前仍需审核完整依赖和对应源码义务。
