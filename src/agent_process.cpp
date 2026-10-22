#include "agent_process.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStringList>
#include <vector>
#include <cmath>
#include <windows.h>

namespace twinfile {

QJsonObject runAgentCommand(const QString &root, const QJsonObject &arguments, const Cancellation &cancel) {
    auto failure = [](const QString &message) { return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}}; };
    if (cancel && cancel->load()) return failure(QStringLiteral("任务已停止"));
    if (arguments.size() != 3 || !arguments.value(QStringLiteral("command")).isString() ||
        !arguments.value(QStringLiteral("working_directory")).isString() || !arguments.value(QStringLiteral("timeout_seconds")).isDouble()) return failure(QStringLiteral("命令参数必须为 command、working_directory 和 timeout_seconds"));
    const QString command = arguments.value(QStringLiteral("command")).toString();
    const double seconds = arguments.value(QStringLiteral("timeout_seconds")).toDouble();
    if (command.trimmed().isEmpty() || command.size() > 8192 || command.contains(QChar::Null) || !std::isfinite(seconds) || seconds < 1 || seconds > 120 || seconds != int(seconds)) return failure(QStringLiteral("命令最多 8K 字符，超时必须为 1–120 整数秒"));
    QString directory = arguments.value(QStringLiteral("working_directory")).toString();
    if (directory.isEmpty()) directory = root;
    else if (QDir::isRelativePath(directory)) directory = QDir(root).absoluteFilePath(directory);
    if (directory.contains(QChar::Null) || !QFileInfo(directory).isDir()) return failure(QStringLiteral("工作目录不存在"));
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH) return failure(QStringLiteral("无法定位 Windows PowerShell"));
    const QString program = QString::fromWCharArray(systemDirectory) + QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe");
    const QString script = QStringLiteral("$ErrorActionPreference = 'Stop'; [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false); $OutputEncoding = [Console]::OutputEncoding; $global:LASTEXITCODE = 0;\n") + command + QStringLiteral("\nexit $LASTEXITCODE\n");
    QByteArray encoded;
    encoded.reserve(script.size() * 2);
    for (const QChar character : script) { encoded.append(char(character.unicode() & 255)); encoded.append(char(character.unicode() >> 8)); }
    const QString commandLine = QStringLiteral("\"%1\" -NoLogo -NoProfile -NonInteractive -EncodedCommand %2").arg(program, QString::fromLatin1(encoded.toBase64()));
    std::wstring mutableCommand = commandLine.toStdWString();
    const std::wstring nativeDirectory = QDir::toNativeSeparators(directory).toStdWString();
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outputRead = nullptr, outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &security, 0)) return failure(QStringLiteral("无法创建命令输出管道"));
    const auto pipes = qScopeGuard([&] { CloseHandle(outputRead); CloseHandle(outputWrite); });
    if (!SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0)) return failure(QStringLiteral("无法配置输出管道"));
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE) return failure(QStringLiteral("无法创建非交互输入"));
    const auto inputGuard = qScopeGuard([&] { CloseHandle(input); });
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return failure(QStringLiteral("无法创建命令进程组"));
    const auto jobGuard = qScopeGuard([&] { CloseHandle(job); });
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return failure(QStringLiteral("无法配置命令停止保护"));
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> storage(attributeSize);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) return failure(QStringLiteral("无法配置进程句柄"));
    const auto attributeGuard = qScopeGuard([&] { DeleteProcThreadAttributeList(attributes); });
    HANDLE inherited[] = {outputWrite, input};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) return failure(QStringLiteral("无法限制继承句柄"));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = outputWrite;
    startup.StartupInfo.hStdError = outputWrite;
    startup.StartupInfo.hStdInput = input;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(reinterpret_cast<LPCWSTR>(program.utf16()), mutableCommand.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nativeDirectory.c_str(), &startup.StartupInfo, &process)) return failure(QStringLiteral("无法启动 Windows PowerShell（错误 %1）").arg(GetLastError()));
    const auto processGuard = qScopeGuard([&] { CloseHandle(process.hThread); CloseHandle(process.hProcess); });
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000);
        return failure(QStringLiteral("无法建立可停止的进程组；命令未执行"));
    }
    if (cancel && cancel->load()) return failure(QStringLiteral("任务已停止"));
    if (ResumeThread(process.hThread) == DWORD(-1)) return failure(QStringLiteral("无法恢复命令进程"));
    QElapsedTimer elapsed; elapsed.start();
    QByteArray output;
    QString error;
    bool finished = false;
    DWORD exitCode = 1;
    constexpr int outputLimit = 65536;
    while (true) {
        if (cancel && cancel->load()) { error = QStringLiteral("任务已停止；命令进程组已终止"); break; }
        if (elapsed.elapsed() >= qint64(seconds * 1000)) { error = QStringLiteral("命令超时；进程组已终止"); break; }
        DWORD available = 0;
        if (!PeekNamedPipe(outputRead, nullptr, 0, nullptr, &available, nullptr)) { error = QStringLiteral("读取命令输出失败"); break; }
        if (available) {
            char block[4096]; DWORD received = 0;
            if (!ReadFile(outputRead, block, qMin<DWORD>(sizeof(block), available), &received, nullptr)) { error = QStringLiteral("读取命令输出失败"); break; }
            output.append(block, received);
            if (output.size() > outputLimit) { output.truncate(outputLimit); error = QStringLiteral("输出超过 64 KiB，命令进程组已终止"); break; }
            continue;
        }
        if (finished) break;
        const DWORD wait = WaitForSingleObject(process.hProcess, 50);
        if (wait == WAIT_OBJECT_0) { finished = true; GetExitCodeProcess(process.hProcess, &exitCode); }
        else if (wait == WAIT_FAILED) { error = QStringLiteral("等待命令失败"); break; }
    }
    TerminateJobObject(job, 1);
    WaitForSingleObject(process.hProcess, 5000);
    QElapsedTimer cleanup; cleanup.start();
    bool groupFinished = false;
    while (cleanup.elapsed() < 5000) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) break;
        if (!accounting.ActiveProcesses) { groupFinished = true; break; }
        Sleep(10);
    }
    if (!groupFinished) error += (error.isEmpty() ? QString() : QStringLiteral("；")) + QStringLiteral("无法确认命令进程组已全部退出");
    if (error.isEmpty() && exitCode != 0) error = QStringLiteral("命令退出码 %1").arg(exitCode);
    QJsonObject result{{QStringLiteral("ok"), error.isEmpty()}, {QStringLiteral("untrusted_output"), QString::fromUtf8(output)}, {QStringLiteral("working_directory"), directory}};
    if (!error.isEmpty()) result.insert(QStringLiteral("error"), error);
    if (finished) result.insert(QStringLiteral("exit_code"), qint64(exitCode));
    return result;
}

}
