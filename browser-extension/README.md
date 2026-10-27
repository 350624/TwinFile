# 浏览器下载接管

适用于 Chrome 和 Edge 的 Manifest V3 扩展。扩展把浏览器下载任务交给独立的 TwinFile Downloader，不会将 Cookie、登录信息或网页内容交给下载器。

## 安装

1. 安装 `TwinFile-Downloader-3.1.24-Windows-x64-Setup.exe`，或运行便携包 `dist/TwinFileDownloader-3.1.24/TwinFileDownloader.exe`。
2. 在浏览器打开扩展管理页面并启用开发者模式，选择“加载已解压的扩展程序”，指向下载器安装目录下的 `browser-bridge` 文件夹。
3. 复制浏览器显示的扩展 ID，然后只注册需要使用的浏览器：

   ```powershell
   powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\Programs\TwinFile Downloader\browser-bridge\register-browser-bridge.ps1" -Browser Chrome -ChromeExtensionId Chrome扩展ID
   ```

   Edge 示例将 `-Browser Chrome -ChromeExtensionId` 替换为 `-Browser Edge -EdgeExtensionId`；若两边都使用，传 `-Browser Both` 并同时提供两个 ID。
4. 重启浏览器。扩展弹窗中“接管浏览器下载”默认关闭；打开后，会先暂停浏览器任务，成功启动 TwinFile 下载器后再取消原任务；启动失败则恢复浏览器下载。右键网页链接可选择“用 TwinFile Downloader 下载链接”，不必开启全局接管。

卸载扩展前关闭接管开关。卸载浏览器扩展不会删除已下载文件；卸载程序后可删除 `HKCU\Software\Google\Chrome\NativeMessagingHosts\com.twinfile.downloader` 和 `HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.twinfile.downloader`。重新安装扩展后若 ID 改变，再运行脚本更新允许列表。

## 范围与注意

- 不处理非 HTTP(S)、浏览器内置页面及扩展自己的下载。
- 扩展没有读取页面 DOM、Cookie 或浏览历史的权限，也不会捕获任意普通链接点击。
- 被接管的请求不继承浏览器 Cookie、Authorization、自定义请求头、代理登录态或 POST 请求体；依赖这些信息的下载应关闭接管并使用浏览器。
- 接管对话框展示完整 URL。下载链接可能带短期签名，避免把链接截图或分享给他人。
- 最高 512 并发是下载器上限，不保证更快；服务器限流时请降低并发。
