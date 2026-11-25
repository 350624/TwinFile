# Miuix 上游与 TwinFile Rust 桌面适配

上游：compose-miuix-ui/miuix；参考提交 03d15786e685f4921fd5638ac56a0802cd27bb5c。上游为 Apache-2.0，版权归 compose-miuix-ui contributors，许可证见 Apache-2.0.txt。

| 已核对的中文指南 | TwinFile 的实现和边界 |
|---|---|
| 快速开始、平台支持、最佳实践 | 上游 Desktop 构件是 Compose/JVM。本项目保持 Windows x64 Rust/egui 及预编译 EXE，不引入 JVM。 |
| 主题系统、颜色系统、文本样式 | 使用上游非 Monet 亮暗色板和语义角色；保留 TwinFile 强调色、用户现有设置和减少动画。工作区用 title1（32），面板沿用 title3、body2、footnote1/2；没有移植 Monet 取色或系统主题监听。 |
| 图标系统 | 搜索图标取形自 MiuixIcons.Basic.Search 的 20×20 视口；TwinFile 的文件类型、磁盘标识和其他命令图标保留。 |
| 工具函数 | 复用 egui 弹层焦点/关闭机制；选项卡有悬停与按压反馈，滚轮正常透传；没有移植触摸 Overscroll 或 Haptics。 |
| 导航支持、窗口与标题栏 | 左右工作区独立保留目录、标签和历史；活跃栏只有一份状态。采用大标题层级与 TabRowWithContour 的选中项模型，不改变 Windows 原生标题栏。 |
| 平滑圆角 | 移植 SquirclePath.kt 的角区扩展、三次曲线及胶囊过渡，在 egui 中用凸多边形绘制标题栏控件、卡片与选中指示；没有依赖 Skia shader。 |
| 模糊效果、材质、玻璃表面 | 上游 Blur / Material / Glass 依赖自身背景采样和渲染后端。本轮保留不透明的高对比表面，没有把半透明填充冒充背景模糊。 |

代码对照：theme/Colors.kt、theme/TextStyles.kt、basic/TabRow.kt、basic/TopAppBar.kt、basic/SearchBar.kt、basic/Card.kt、basic/TextField.kt、basic/NavigationBar.kt（FloatingNavigationBar）、overlay/OverlayDialog.kt、basic/ListPopup.kt、icon/basic/Search.kt、miuix-squircle 中的 SquirclePath.kt。组件层位于 src/theme/miuix.rs，工作区调用在 src/ui/workbench.rs。工作区统一底色，并使用同层级卡片、居中的浮动操作胶囊、独立的搜索弹层和结果层。移除左下重复入口；更多菜单保留原有功能，最近的位置迁入其中。不改用户的亮暗选择。

验证：package-preview.ps1 在 1180 与 760 像素窗口中点击左右标题按钮和栏内空白，要求每次都切换真实活动栏；保留双宽度主题、目录动效、菜单和设置重启测试。
弹层验证包括：点击图标后输入框获焦、Escape 关闭、Enter 提交、打开搜索结果和点击底部外观按钮。
