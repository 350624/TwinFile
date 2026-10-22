# 发布与后续更新

## 日常流程

正式开发目录是本 Git 仓库。每次软件更新应：修改功能及测试 → 更新版本和 `CHANGELOG.md` → 检查差异/敏感信息 → 显式暂存相关文件并提交 → 运行发布脚本。`AGENTS.md` 已记录这一收尾要求，避免以后只上传源码而遗漏安装包。

`publish.ps1` 自动完成：
1. 校验 main、干净工作区、origin、CMake/安装器版本及对应更新日志；拒绝覆盖已发布的不同提交。
2. 完整构建和四组回归，部署 Qt / 工具 / 通知，运行 Windows UI 与独立便携启动检查。
3. 生成安装器、对应提交的源码 ZIP、组件清单与 `SHA256SUMS.txt`。
4. 普通推送 main 和 `v版本号` 标签，不强推；创建草稿 Release，逐项上传并核对服务端 SHA256。
5. 全部资产完整后公开 Release，正文取自该版本更新日志。失败保留草稿，不发布空的正式版本。

```powershell
.\publish.ps1 -QtRoot 'C:\Qt\6.10.3\mingw_64' `
  -CompilerRoot 'C:\Qt\Tools\mingw1310_64' `
  -ToolRoot 'C:\TwinFileTools' `
  -CMake 'C:\Program Files\CMake\bin\cmake.exe' `
  -Compiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
```

需 PowerShell 7、Git、完整构建工具和 GitHub 仓库写权限。先通过 Git Credential Manager 登录 GitHub；无人值守环境可在秘密存储中提供 `GH_TOKEN` / `GITHUB_TOKEN`，不得写进仓库。脚本不要求安装 gh。`-CheckOnly` 只进行本地发布条件检查，不构建、不联网、不上传。

二进制资产命名为 `TwinFile-版本-Windows-x64-Setup.exe`，源码为 `TwinFile-版本-source.zip`。GitHub 自带的 Source code 是源码，不是安装器。安装包使用固定本地输出名，发布时另取包含版本号的名称。

## 失败恢复与边界

- 不复用已公开的版本号；发布脚本发现同一提交已完整公开时仅返回现有链接。变更源码必须递增版本。
- 同版本草稿可重跑；脚本仅允许替换该草稿中本次预期的同名资产，以便构建时间戳变化后恢复上传，不动其它资产。已公开资产永不覆盖。
- 推送成功但上传失败时，main / 标签仍可能已在远端。修好认证或网络后，用完全相同的提交重试；有新修改则另发版本。
- `-CheckOnly` 不能替代真正构建、设备测试或安装验收。发布失败不会回滚已推送源码；不要删除标签掩盖失败。
- 该流程按发布命令运行；没有定时任务或后台文件监控。今后在本项目继续修改时按 `AGENTS.md` 自动收尾，不会擅自上传其它目录内容。
- 此处“自动更新仓库”不是客户端自动更新器；已安装软件不会静默下载安装新版。

依赖来源和再分发通知见 [THIRD_PARTY.md](../THIRD_PARTY.md) 与 [SOURCES.md](../licenses/SOURCES.md)。发布者仍需持续维护具体版本的许可与源码义务，不应把组件版本清单当成法律或安全审计证明。
