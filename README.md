# TwinFile Rust · 3.1.44 功能迁移工程

这是独立的 Windows x64 Rust 工程，不加载旧 TwinFile EXE、Qt 或 Python。旧源码、旧程序与旧配置保留原样。当前产物为开发预览，不是旧版的完整替代品；以 [功能对照表](PARITY.md) 为准。

## 直接启动

解压预览包进入新目录，双击 TwinFileRust.exe；也可直接运行本工程 dist 目录下的可执行文件。TwinFileRustDownloader.exe 直接开启下载器界面。两个入口都不需要在运行环境安装 Rust 或 Qt，不注册浏览器和启动项，不创建磁盘映射。工作台配置位于 %APPDATA%\TwinFileRust\settings.json，独立下载器使用 downloader 子目录。可选 Chrome / Edge 下载接管见便携包 browser-bridge/README.md，不自动注册。

Miuix 可在底部胶囊的「外观设置」或「⋯ → 设置 → 外观设置」启用。Miuix 的下载和收藏仍在「⋯」中，「最近的位置」也可从这里打开。左侧「此电脑」在当前栏列出 Windows 卷；双击磁盘进入目录，已打开的卷列表标签会随工作区保存。卷列表本身不是文件夹，不能作为复制、移动或粘贴目标。

目录下载失败时，界面显示创建临时文件时的真实错误。选择文件名不会修改权限、创建空目标或自动改用其他目录。

直启动版后续就地更新：先关闭正在运行的文件管理器，再把验证通过的 Release GUI 主程序覆盖到桌面「TwinFile Rust（预览版）」快捷方式当前指向的 TwinFileRust.exe。直启动版更新不新增发布目录、压缩包或快捷方式，不覆盖独立下载器、扩展或用户配置。

## 两项传输工具

- **更多（⋯）→ 本地 / 远程 Payload 分区提取**：选本地 `payload.bin` / OTA ZIP 或输入 HTTP(S) 直链，点「读取分区」，勾选分区、指定输出目录并提取。不会刷机或覆盖已有镜像。支持稳定 Range 和强 ETag 时按需读取；否则需在临时目录缓存整包，ZIP 内压缩的 payload 也需要临时空间。依赖旧镜像的增量操作不支持；直链默认不保存。
- **更多（⋯）→ HFX USB + WiFi 多轨传输**：USB 方式请先在手机确认 USB 调试，刷新并选中设备；未装配套手机端时点「安装配套手机端」并在手机授予文件权限。默认「USB + Wi-Fi」模式要求选中已授权的 USB 手机和电脑的局域网 IPv4，确认网络可信后点「连接双通道」；也可显式切换到「仅 USB」或「仅 Wi-Fi」。仅 WLAN 时需手动在手机端连接电脑。连上后浏览手机目录，发送文件/文件夹，或选中条目接收到电脑。HFX v300 的网络通道不加密且没有强身份验证，仅用于可信局域网。不会自动更改 Windows 防火墙。双通道已在本机模拟手机往返测试，未进行实体手机 USB + Wi‑Fi 联测。

## 从源码构建（开发者入口）

日常使用请打开桌面的 TwinFile Rust 预览版快捷方式，不会弹终端或重新编译。开发时可双击 Start-TwinFile-Rust.cmd；它会用已配置的 Rust 工具链构建当前源码，然后启动新的 TwinFile Rust，不读写旧 TwinFile 工程。只编译可从终端传入 --build-only。也可在 PowerShell 7 用 ./start-source.ps1。

## 本机复现

在本目录用 PowerShell 7 运行：

    . ./toolchain.ps1
    cargo fmt --check
    cargo test --locked -- --test-threads=1
    node tests/browser_mock.cjs
    ./package-preview.ps1

MinGW 不兼容本机中文链接路径，Rust 工具链、Cargo 缓存和输出放在 D:\TwinFile-Rust-Build。源码和便携包不必搬离中文目录；不需要 SUBST、网络映射或改 ACL。换电脑时按实际工具位置传 -BuildRoot、-GccBin，使用 Rust x86_64-pc-windows-gnu 工具链。

打包需要 PowerShell 7、Node.js 与可运行桌面 GUI 的 Windows 会话。发布脚本会验证两次设置启动、TwinFile、Miuix、柔和及发布会四套主题的亮暗和双窗口宽度、Miuix 菜单、左右栏实点切换、搜索和底部外观入口、真实原生下载桥接，并将记录写入 verification。

Miuix 设计参照 Compose Miuix 提交 03d15786e685f4921fd5638ac56a0802cd27bb5c，使用 Rust/egui 实现，见 MIUIX-NOTICE.md。

「外观设置 → 柔和」以雾灰画布、白色圆角卡片和靛蓝交互色组织工作台。窄窗侧栏收为图标；悬停显示位置名称。正文优先本机 Inter，其次 Segoe UI，中文使用系统回退字体；支持暗色及减少动画。未修改现有用户的主题选择。

「外观设置 → 发布会」：固定纯黑、电蓝、石墨双栏；当前位置索引、工具气泡和滚动联动的双盘矢量舞台。窄窗缩为图标导航；开启减少动画时舞台静止。不会覆盖其他主题的明暗及强调色偏好。
