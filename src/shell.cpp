#include "shell.h"
#include <QDir>
#include <QFileInfo>
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>

namespace twinfile {
BatchPlan batchPlan(const QString &path) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.isJunction() || (info.suffix().compare(QStringLiteral("bat"), Qt::CaseInsensitive) != 0 && info.suffix().compare(QStringLiteral("cmd"), Qt::CaseInsensitive) != 0)) return {};
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    BatchPlan plan;
    plan.program = QDir(QString::fromWCharArray(system)).filePath(QStringLiteral("cmd.exe"));
    plan.command = QChar('"') + QDir::toNativeSeparators(plan.program) + QStringLiteral("\" /d /s /v:off /k \"\"%TWINFILE_BATCH%\"\"");
    plan.directory = info.absolutePath(); plan.environment = QProcessEnvironment::systemEnvironment();
    const QString tools = QFileInfo(toolPath(QStringLiteral("adb/adb.exe"))).absolutePath();
    plan.environment.insert(QStringLiteral("PATH"), QDir::toNativeSeparators(tools) + QChar(';') + plan.environment.value(QStringLiteral("PATH")));
    plan.environment.insert(QStringLiteral("TWINFILE_BATCH"), QDir::toNativeSeparators(info.absoluteFilePath()));
    return plan;
}
Result launchBatch(const QString &path) {
    const BatchPlan plan = batchPlan(path);
    if (plan.program.isEmpty()) return {false, QStringLiteral("请选择本地普通 .bat / .cmd 文件"), {}, {}};
    QString environment;
    auto variables = plan.environment.toStringList(); variables.sort(Qt::CaseInsensitive);
    for (const QString &variable : variables) { environment += variable; environment += QChar(0); }
    environment += QChar(0);
    QString command = plan.command;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(reinterpret_cast<LPCWSTR>(plan.program.utf16()), reinterpret_cast<LPWSTR>(command.data()), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, const_cast<ushort *>(environment.utf16()), reinterpret_cast<LPCWSTR>(plan.directory.utf16()), &startup, &process);
    if (!started) return {false, QStringLiteral("无法启动脚本控制台，Windows 错误：%1").arg(GetLastError()), {}, {}};
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return {};
}
QImage shellThumbnail(const QString &path, const Cancellation &cancel) {
    if ((cancel && cancel->load()) || !QFileInfo(path).isFile()) return {};
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
    IShellItemImageFactory *factory = nullptr; QImage image;
    const QString nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const HRESULT status = SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(status) && factory) {
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(factory->GetImage(SIZE{96, 96}, SIIGBF(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY), &bitmap)) && bitmap) {
            BITMAP descriptor{};
            if (GetObjectW(bitmap, sizeof(descriptor), &descriptor) && descriptor.bmWidth > 0 && descriptor.bmHeight > 0 && descriptor.bmWidth <= 512 && descriptor.bmHeight <= 512) {
                image = QImage(descriptor.bmWidth, descriptor.bmHeight, QImage::Format_ARGB32); image.fill(Qt::transparent);
                BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = image.width(); info.bmiHeader.biHeight = -image.height(); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
                const HDC device = GetDC(nullptr); if (!GetDIBits(device, bitmap, 0, UINT(image.height()), image.bits(), &info, DIB_RGB_COLORS)) image = {}; ReleaseDC(nullptr, device);
                bool alpha = false; for (int row = 0; row < image.height() && !alpha; ++row) { const auto *pixels = reinterpret_cast<const QRgb *>(image.constScanLine(row)); for (int column = 0; column < image.width(); ++column) if (qAlpha(pixels[column])) { alpha = true; break; } }
                if (!alpha) for (int row = 0; row < image.height(); ++row) { auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(row)); for (int column = 0; column < image.width(); ++column) pixels[column] |= 0xff000000; }
            }
            DeleteObject(bitmap);
        }
        factory->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize(); return cancel && cancel->load() ? QImage() : image;
}
}
