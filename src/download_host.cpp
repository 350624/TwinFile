#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QUrl>
#include <QStringList>

#include <windows.h>

#include <array>

namespace {
int respond(const QByteArray &json) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const quint32 size = quint32(json.size());
    const std::array<char, 4> header{char(size & 0xff), char((size >> 8) & 0xff), char((size >> 16) & 0xff), char((size >> 24) & 0xff)};
    DWORD written = 0;
    if (!WriteFile(output, header.data(), DWORD(header.size()), &written, nullptr) || written != header.size()) return 7;
    if (!WriteFile(output, json.constData(), size, &written, nullptr) || written != size) return 8;
    return 0;
}

bool readExact(HANDLE input, char *buffer, DWORD size) {
    DWORD total = 0;
    while (total < size) {
        DWORD received = 0;
        if (!ReadFile(input, buffer + total, size - total, &received, nullptr) || received == 0) return false;
        total += received;
    }
    return true;
}

QString quoteArgument(const QString &argument) {
    QString quoted = QStringLiteral("\"");
    qsizetype slashes = 0;
    for (const QChar character : argument) {
        if (character == u'\\') { ++slashes; continue; }
        if (character == u'"') quoted += QString(slashes * 2 + 1, u'\\') + character;
        else quoted += QString(slashes, u'\\') + character;
        slashes = 0;
    }
    quoted += QString(slashes * 2, u'\\') + u'"';
    return quoted;
}
}

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    std::array<unsigned char, 4> header{};
    if (!readExact(input, reinterpret_cast<char *>(header.data()), DWORD(header.size()))) return 1;
    const quint32 size = quint32(header[0]) | (quint32(header[1]) << 8) | (quint32(header[2]) << 16) | (quint32(header[3]) << 24);
    if (size == 0 || size > 65536) return 2;
    QByteArray payload(int(size), Qt::Uninitialized);
    if (!readExact(input, payload.data(), size)) return 3;
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) return 4;
    const auto message = document.object();
    const QUrl url(message.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
    QString filename = QFileInfo(message.value(QStringLiteral("filename")).toString()).fileName();
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment()) return respond(R"({"accepted":false})");
    if (filename.isEmpty()) filename = QFileInfo(url.path()).fileName();

    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/TwinFileDownloader.exe");
    QString command = quoteArgument(QDir::toNativeSeparators(executable)) + QStringLiteral(" --url ") + quoteArgument(url.toString(QUrl::FullyEncoded)) + QStringLiteral(" --filename ") + quoteArgument(filename);
    std::wstring mutableCommand = command.toStdWString();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.toStdWString().c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr, QCoreApplication::applicationDirPath().toStdWString().c_str(), &startup, &process)) return respond(R"({"accepted":false})");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return respond(R"({"accepted":true})");
}
