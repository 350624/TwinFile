# Miuix Console 原生适配

- 上游：<https://github.com/bmbxwbh/miuix-console>
- 固定提交：`e7b5807ae7d4e4045b3fd8ec31b0499459476822`
- 作者/项目署名：bmbxwbh / Miuix Console。
- 许可：上游该提交的 `README.md` 明确声明 Apache License 2.0；该提交未附独立 LICENSE 或 NOTICE 文件。本仓库保留原 README，协议全文见 [Apache-2.0.txt](Apache-2.0.txt)。不代替上游补写版权声明。
- 保留来源：`vendor/miuix-console` 中的 README、设计令牌、按钮/导航/输入框/标签/浮层 CSS，以及圆角/涟漪 JS 均为此提交的未修改副本，用于核对来源，不在运行时加载。
- `core/tokens.css` 的 Git 源文件 SHA256（UTF-8、LF）：`65020174f9d64bdc78733a2f122b85268803c2791f3205e5c7a9a6a88738b9f8`。Windows 检出转换为 CRLF 时文件哈希不同。

## 本项目修改

TwinFile 3.1.40 将上游五级 surface、主色/状态色、字号/尺寸和缓动参数移植为 `src/miuix_theme.cpp` 的原生 Qt 设计令牌；`src/miuix.cpp`、`smooth_widgets.cpp`、`motion.cpp`、`model.cpp` 为原生组件与交互适配。原许可继续覆盖来自上游的部分，其余原创修改沿用 TwinFile 的 GPL-3.0-only。

- 保留原生双栏、路径编辑、文件选择/传输、进度、扩展助手、键盘操作和无障碍，不运行网页代码，不依赖 CDN、在线字体或浏览器内核。
- 采用上游普通 desktop/button/input/card-tab/pill/popover 设计，不引入其可选 Liquid Glass 折射效果，也未实现 CSS backdrop-filter 的背景模糊。桌面顶栏使用等价不透明层级，未将普通半透明冒充原生毛玻璃。
- 字体优先使用本机 MiSans/Mi Sans，没有安装时回退 Microsoft YaHei UI；不打包或下载 MiSans 字体。
- 上游 `smooth-corner.js` 的单段三次曲线近似圆弧，直线连接点曲率非零。根据用户视频要求，原生角改用每角两段、连接点一二阶导数匹配且直线端曲率为零的曲线；不是声称逐像素复制上游圆角。
- 按钮移植按压缩放和裁剪涟漪；不改变布局尺寸或点击区域。弹性只用于侧栏指示条的物理反馈，其余采用标准减速曲线。“减少动画”、隐藏、禁用和主题切换会停止未完成反馈。

本适配不代表与 Miuix、Miuix Console 或 Xiaomi/HyperOS 存在隶属、合作或背书关系。
