# TwinFile Rust · 浏览器下载接管

1. 在 Chrome 或 Edge 的扩展管理页开启开发者模式，加载便携包中的 browser-bridge 目录并复制扩展 ID。
2. 在包目录打开 PowerShell 7，运行 ./browser-bridge/register.ps1 -Browser Chrome -ExtensionId <扩展ID>，或将 Chrome 换成 Edge。注册只应用于 HKCU 的 Rust 专用主机名，不更改旧版桥接。重新解压到新目录之后需要重新注册。
3. 扩展全局接管默认关闭。打开后先暂停浏览器任务，Rust 桥接确认成功交给新下载器窗口后再取消原任务；交接失败恢复原任务。也可右键链接单独发送。
4. Rust 下载器显示链接和建议文件名，仍须用户选择本地保存位置并点击开始；不会继承 Cookie、登录状态、POST 数据或自定义请求头。链接不写配置，不作为命令行参数。
5. 关闭全局接管并卸载扩展后，按相同浏览器参数调用 register.ps1 -Browser Chrome -Unregister。

发布流程检查本地原生消息帧、下载器窗口确认与浏览器 API 模拟。尚未在 Chrome 和 Edge 进行实际浏览器联动验收。安装脚本不会自动运行。
