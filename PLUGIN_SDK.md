# TwinFile 扩展 SDK · API v1

## 这套框架能做什么

本机 C++ / Qt 插件可贡献右侧面板、设置对话框和命令。内置 AI 也通过相同的激活、面板创建和停用接口接入，后续新功能无需继续堆在主窗口内。当前不含线上商店、插件自动更新、脚本运行时、VS Code 扩展兼容层或进程隔离沙箱。

## 用户操作

1. 顶部“助手”按钮，或 `Ctrl+Shift+A` 打开侧栏，可拖动左右分界线调整宽度，点侧栏右上角关闭可收起；左下角“扩展与工具”直接打开扩展管理页。
2. “对话”页右上角齿轮打开接口设置，填写基础地址、API Key，点击“获取模型”后下拉选择，也可手动填写模型 ID。输入框内“＋”添加附件，下方选择聊天/Agent，向上箭头发送、方块停止；`Ctrl+Enter` 发送，Enter 换行。软件不提供密钥、额度或代理服务。
3. “扩展 → 安装本地插件…”选 `.dll`，检查名称、ID、版本、SHA256 和来源，确认信任后复制安装。插件初始停用，选中后“启用 / 停用”。
4. 启用的插件若提供面板，会新增同名标签；只提供命令的插件可从“执行所选插件命令…”调用。“设置”交给插件处理。
5. 卸载取消信任并停用。Windows 已加载 DLL 通常要到进程退出才释放；磁盘副本保留但不会被扫描执行，需要清理时请退出应用后处理。升级同 ID 插件需先卸载、退出重启，再安装新版本。

内置对话页采用 Codex 风格的底部组合输入区，右上角“更多”包含复制回答与隐私说明。工具步骤默认折叠，点击展开只切换本地显示，不触发网络或执行操作；模型输出仍转义为文字，不打开回复中的链接。

## AI 接口约定

普通聊天适配器使用 HTTP POST `chat/completions`、`Authorization: Bearer <key>`、JSON `model/messages/stream: true`。接受 SSE 的 `choices[0].delta.content` 和 `[DONE]` / `finish_reason: stop`，也接受完整 JSON 的 `choices[0].message.content`。聊天不发送工具定义、不执行工具调用；需要操作文件时使用下述 Agent 模式。多模态或专用 Responses API 仍需另写适配器。

基础地址示例：

- `https://api.deepseek.com` → `https://api.deepseek.com/chat/completions`，模型名称由用户按其服务填写。
- `https://api.example.com/v1` → `https://api.example.com/v1/chat/completions`。
- `http://127.0.0.1:1234/v1` → 本机 OpenAI 兼容服务；免密时 Key 可留空。
- 已填写完整 `/chat/completions` 端点时不会再重复追加。

“获取模型”仅由用户点击触发，向同一基础地址发送 GET `/models`，复用 Bearer Key，解析 OpenAI 兼容的 `data[].id`，去重并排序。若地址已包含 `/chat/completions`，先移除该后缀再请求 `/models`。获取不发送聊天内容/文件/系统提示词，也不会自动保存或更换当前聊天模型；下拉框始终可手填。服务不支持、无权限、空列表或格式不兼容时显示提示，不阻止手填。

模型请求异步，15 秒无传输超时、45 秒总时限，最大 2 MiB / 4096 条目 / 每个 ID 256 字符。可再次点击取消，修改地址/Key 会取消旧请求并清除旧选项（保留手填内容）；关闭设置窗中止请求。保持 HTTPS/本机 HTTP 限制、TLS 验证和禁止重定向转发密钥。只有保存设置后才应用新模型。

远程只允许 HTTPS，本机 `localhost`、`127.0.0.1`、`::1` 可 HTTP。不接受 URL 内账号、查询参数、片段；不支持 Azure 专用查询参数/认证格式。禁止重定向转发密钥，不忽略 TLS 错误。网络调用异步，60 秒无传输超时、总时限 180 秒，允许手动停止；不会自动重试。

输入单次最多 16K 字符，系统提示词 8K；单会话最多 20 轮，发送的总上下文最多 128K 字符，单次响应最多 2 MiB，回答文字最多 128K 字符。触限明确报错，不静默裁掉已发送的对话。计量是字符，不是服务端 token，仍可能触及服务商的上下文限制或费用。

“所选名称”只附加最多 256 个文件名，不含路径和内容；“文本文件”只读取用户自行选择的有效 UTF-8 文本，最大 64 KiB。不支持自动读取手机/压缩包内容。附件先预览再确认，下次发送才出网，每条消息一个附件；成功发送后从待附加区清除，但作为该轮对话的一部分，后续提问仍会携带历史附件，直到新建对话或变更配置。修改接口、模型、密钥、系统提示词会开启新对话，避免旧历史发给新服务。

聊天内容不写磁盘；设置在 `%APPDATA%/TwinFileNative/ai.json`，API Key 默认只在内存，记住后使用 Windows DPAPI 加密，不写入 `settings.json` 或日志。关闭“记住”并保存会删除旧密文，清空 Key 并保存可忘记密钥。DPAPI 不能防御同账户恶意软件或已加载的可信插件。地址、模型和系统提示词是普通设置，请不要把密钥写进这些字段。

## 内置 Agent 协议与授权

侧栏明确切换 Agent 后选择本机目录并确认隐私提示，再点击“开始任务”才联网。权限下拉框默认是“修改前确认”；“完全权限”必须由用户在风险对话框中主动开启，模型不能自行提升。完全权限只在当前对话有效，每次任务启动时固定权限快照，允许受控 PowerShell 以当前 Windows 账户访问工作目录外文件、写入/覆盖/删除、运行程序及联网，不自动提权、不逐项确认；新对话、切换模式、修改接口或重启会恢复默认。默认模式目录授权与完全权限工作目录分开保存；降级或新对话仅恢复此前明确授权的目录，未授权则保持空白。授权和权限只在当前面板内存中保留；默认模式不自动附加当前栏、已选路径或绝对授权目录，完全权限会发送工作目录供命令使用。每次发送独立执行，不携带聊天/上一任务历史；手动预览附加的数据仍可随任务发送。工具结果会发给当前用户配置的服务。

请求沿用相同端点和 Bearer，使用 `stream: false`、`tools`（标准 function JSON schema）、`tool_choice: auto`。接收 `choices[0].message.tool_calls`，要求 `finish_reason: tool_calls`、唯一非空 ID、function 类型和有效 JSON 对象参数；以 `assistant.tool_calls` 和对应 `role: tool/tool_call_id/content` 发回后续请求，直到 `finish_reason: stop` 返回最终文字。不解析自然语言来执行操作，不接受截断响应、重复 ID 或非请求的 SSE 工具片段；服务需支持非流式标准工具协议。不自动重试或自行降级执行。

| 工具 | 能力与上限 |
| --- | --- |
| `list_directory(path)` | 相对路径，根用 `.`；最多 200 项，不含隐藏项目，不跟随重解析点 |
| `search_names(path, query)` | 按名称包含匹配，不读内容；最多检查 2000 项、深度 6、返回 100 项，标记截断 |
| `file_info(path)` | 类型、大小、修改时间，不返回绝对路径 |
| `read_text(path)` | 最多 64 KiB，无 NUL 的有效 UTF-8，输出标记为不可信参考数据 |
| `sha256(path)` | 单文件不超过 64 MiB，后台分块计算，可取消 |
| `create_folder(path)` | 单层新建，不覆盖，每次弹窗确认 |
| `create_text(path, content)` | 新建 txt/md/json/csv/log/ini/yaml/yml/xml，最多 64 KiB；完整内容先预览，不覆盖 |
| `rename_entry(path, new_name)` | 仅同目录普通文件，不重命名目录、不覆盖；目标完整路径先确认 |

默认模式拒绝额外参数、绝对路径、`..`、UNC/映射网络盘、ADS、保留文件名、符号链接和连接点；读取/哈希/重命名拒绝多重硬链接文件。新建使用 NewOnly，重命名用不带替换标志的 Windows 同卷移动，确认后重新验证路径和目标存在性。完全权限额外提供 `run_command(command, working_directory, timeout_seconds)`：PowerShell 非交互运行，命令最多 8K 字符、1–120 秒、输出最多 64 KiB；使用 Windows Job Object 终止子进程，结束时等待进程组退出，无法确认清理时返回错误。原生命令的最后退出码会保留，后续普通文字输出不会把失败变成成功；PowerShell 终止异常同样返回失败。其余文件工具仍只接受工作目录内相对路径。完全权限不是系统沙箱，也不防御同账户恶意进程、并发路径替换、外部服务或原生 DLL；文件内容和命令输出不可信，提示词防注入不能替代用户核对。

最多 12 次工具调用（同批逐项串行）、512 KiB 序列化上下文、2 MiB 单响应、128K 回答文字。每次网络 60 秒无传输 / 180 秒总时限，整个任务 10 分钟（含等待确认）。目录/读取/哈希在后台，确认弹窗异步；等待时仍算忙碌，不能修改设置或切模式。拒绝会作为工具错误告知模型；停止取消网络、协作取消读取、关闭确认窗，不执行后续步骤，等待后台工具清理后才报告结束并允许新任务。停止不能撤销已完成修改或追回已交给外部服务、计划任务等系统的操作。不支持的模型明确提示，可手动切回普通聊天。任务/工具记录不写磁盘。

修改接口、模型、密钥或系统提示词会清空会话并撤销目录授权，需再次确认才能访问文件。Agent 是内置 C++ 实现，沿用原扩展生命周期，不向外部 `Host` v1 添加虚函数，不改变插件 ABI；它不是授予第三方插件的工具权限系统。

## 开发环境与最小示例

- Windows x64，**Qt 6.10.3 MinGW 64-bit / MinGW GCC 13.1 / C++20 Release**，需匹配宿主 ABI。其它 Qt/编译器/架构不保证二进制兼容。
- `sdk/twinfile_plugin.h`：公共接口，不需要链接主程序或业务静态库。
- `sdk/example/`：完整“选择摘要”插件，读取选择、显示面板、刷新活动栏，注册两个命令，不联网。
- 便携/安装包内 `plugin-examples/selection-example.dll` 是同源码预编译样例，**不会自动加载**，可按上述用户操作安装体验。

独立构建（按实际安装位置调整路径）：

```powershell
$env:Path = 'D:\Qt\Tools\mingw1310_64\bin;D:\Qt\6.10.3\mingw_64\bin;' + $env:Path
cmake -S sdk/example -B build-example -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/Qt/6.10.3/mingw_64
cmake --build build-example --parallel 4
```

工程本身也构建测试用样例 `build/bin/twinfile_example_plugin.dll`。插件交付物当前为单个 DLL；额外依赖不会自动下载或安装，请优先只用随宿主提供的 Qt 模块。不要替换宿主的 Qt DLL，不要把密钥或用户文件打进插件。

## 清单

使用 `Q_PLUGIN_METADATA(IID TwinFilePlugin_iid FILE "example.json")` 和 `Q_INTERFACES(twinfile::extensions::Plugin)`；JSON 编译进 DLL，而不是运行时旁加载未签名清单。

```json
{
  "Id": "yourcompany.feature",
  "Name": "功能名称",
  "Version": "1.0.0",
  "ApiVersion": 1,
  "Description": "插件用途与行为说明"
}
```

ID 限 100 字符，小写英文字母/数字，以 `.` 或 `-` 分段，至少两段；`twinfile.` 为宿主保留。名称 1–80 字符，版本 1–32 字符，说明最多 2000 字符。只接受 Qt 元数据中 IID 和 ApiVersion 匹配的插件；64 MiB 单 DLL 上限，最多安装 64 个外部插件。不支持跨版本 ABI 自动适配。

## 接口与生命周期

所有回调位于 GUI 线程，**不得阻塞**。网络用 Qt 异步；CPU/IO 工作放后台，结果回 GUI 更新控件；离开插件时必须停止任务并释放资源。

| 接口 | 约定 |
| --- | --- |
| `Plugin::activate(Host *, QString *error)` | 获取非拥有的 Host 指针，成功返回 true；失败填写错误并可安全停用。不要在这里自动联网/上传用户数据。 |
| `Plugin::createPanel(QWidget *parent)` | 返回宿主拥有的 QWidget，或返回 nullptr 表示仅命令插件。每次启用创建一次。 |
| `Plugin::commands() const` | 最多 100 个 `{id,title}` 命令；在扩展管理页展示，不注入全局快捷键。 |
| `Plugin::execute(commandId)` | 处理自己注册的命令；影响文件或联网前由插件提供明确说明及确认。 |
| `Plugin::configure(QWidget *parent)` | 可选的插件自有设置对话框。不要阻塞线程或破坏宿主设置。 |
| `Plugin::deactivate()` | 幂等，停止后台工作、断开 Host 引用。面板会先由宿主删除，不要保留悬空 QWidget 指针；建议使用 QPointer。 |
| `Host::apiVersion()` | 当前返回 1。 |
| `Host::selectionContext()` | 当前活动栏的只读上下文，结构见下方；不会主动读取文件内容。 |
| `Host::refreshActivePane()` | 请求刷新当前活动文件栏，不改变选择到另一栏。 |
| `Host::showStatusMessage(text)` | 显示短暂状态文字，不弹模态窗口；最多 1000 字符。 |

Host 指针只在插件激活期间有效；仅在 GUI 线程调用。面板切换/侧栏收起不等于停用；停用插件会关闭面板，内置 AI 会取消进行中的请求且清空会话，重新启用时重建面板。密钥若未记住，仍只在当前进程内存中保留到退出，方便再启用。

选择上下文示例：

```json
{
  "location": { "source": 0, "path": "C:/Work", "inner": "", "label": "" },
  "selected": [
    { "name": "notes.txt", "path": "C:/Work/notes.txt", "inner": "", "source": 0, "directory": false, "size": 128 }
  ],
  "truncated": false
}
```

最多返回 256 项。`source` 为 `Local=0, Drive=1, Archive=2, Device=3, Phone=4`，不能假定每个路径均为本机路径；尺寸为 JSON 数字，极大整数应另外读取校验。不含文件内容、设备授权令牌、AI 密钥；SDK 不暴露执行 shell/删除文件接口。但原生插件本身可绕过 SDK 访问系统，接口约束不等于安全沙箱。

## 信任与加载边界

元数据预览使用 `QPluginLoader::metaData()`，安装前不执行 DLL。用户确认显示的 SHA256 后，复制到 `%APPDATA%/TwinFileNative/extensions/<id>-<sha256>.dll`，指纹再次一致才安装，记录在同目录 `index.json`；安装并不启用。启动只恢复索引内已启用插件，重新验证内容哈希，篡改则拒绝；不扫描任意 DLL，不解析插件目录之外的路径。

SHA256 是内容变化检查，**不是作者签名或安全审计**。索引位于当前 Windows 用户目录，同账户恶意软件仍可修改它；DLL 进程内运行，可访问本机文件/网络/进程内密钥，也可能崩溃宿主。捕获 C++ 异常不能隔离内存错误或恶意代码。真正的不可信插件隔离需要未来另做子进程/RPC/权限沙箱，不在本版承诺范围。

## 验证

`build.ps1` 运行原生、迁移、AI/插件和 Agent 四组 QtTest。本机模拟 HTTP/SSE，不调用真实付费服务，不使用用户 Key；验证 DPAPI、分片 UTF-8、取消、错误、重定向拒绝、大小上限、插件信任/生命周期、标准工具闭环、逐项确认、越界/链接拒绝、不覆盖与聊天隔离。`verify-ui.ps1` 以 Windows 平台隐藏窗口生成真实字体截图，包括 Agent 授权和修改预览。自动化通过不代表任意服务商或任意第三方 DLL 已兼容。
