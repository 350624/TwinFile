#include "zip.h"

#include <QBuffer>
#include <QStringDecoder>
#include <limits>
#include <minizip/unzip.h>
#include <minizip/zip.h>

namespace twinfile {

static zlib_filefunc64_def callbacks(QIODevice *device) {
    zlib_filefunc64_def functions{};
    functions.opaque = device;
    functions.zopen64_file = [](void *opaque, const void *, int) -> void * { return opaque; };
    functions.zread_file = [](void *, void *stream, void *buffer, uLong size) -> uLong {
        const qint64 count = static_cast<QIODevice *>(stream)->read(static_cast<char *>(buffer), size);
        return count < 0 ? 0 : uLong(count);
    };
    functions.zwrite_file = [](void *, void *stream, const void *buffer, uLong size) -> uLong {
        const qint64 count = static_cast<QIODevice *>(stream)->write(static_cast<const char *>(buffer), size);
        return count < 0 ? 0 : uLong(count);
    };
    functions.ztell64_file = [](void *, void *stream) -> ZPOS64_T { return static_cast<QIODevice *>(stream)->pos(); };
    functions.zseek64_file = [](void *, void *stream, ZPOS64_T offset, int origin) -> long {
        auto *device = static_cast<QIODevice *>(stream);
        if (offset > ZPOS64_T(std::numeric_limits<qint64>::max())) return -1;
        const qint64 base = origin == ZLIB_FILEFUNC_SEEK_END ? device->size() : origin == ZLIB_FILEFUNC_SEEK_CUR ? device->pos() : 0;
        if (base > std::numeric_limits<qint64>::max() - qint64(offset)) return -1;
        return device->seek(base + qint64(offset)) ? 0 : -1;
    };
    functions.zclose_file = [](void *, void *) -> int { return 0; };
    functions.zerror_file = [](void *, void *stream) -> int {
        const auto *file = dynamic_cast<QFileDevice *>(static_cast<QIODevice *>(stream));
        return file && file->error() != QFileDevice::NoError;
    };
    return functions;
}

ZipReader::ZipReader(const QString &path) : file_(path) {
    if (!file_.open(QIODevice::ReadOnly)) { error_ = file_.errorString(); return; }
    auto functions = callbacks(&file_);
    handle_ = unzOpen2_64("qt", &functions);
    if (!handle_) { error_ = QStringLiteral("ZIP 中央目录损坏或不支持"); return; }
    unz_global_info64 global{};
    if (unzGetGlobalInfo64(handle_, &global) != UNZ_OK || global.number_entry > 200000) {
        error_ = QStringLiteral("ZIP 条目数量过大或损坏"); return;
    }
    int status = unzGoToFirstFile(handle_);
    for (quint64 index = 0; index < global.number_entry; ++index) {
        unz_file_info64 info{};
        if (status != UNZ_OK || unzGetCurrentFileInfo64(handle_, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK
            || info.size_filename > 32768 || info.uncompressed_size > quint64(std::numeric_limits<qint64>::max())) {
            error_ = QStringLiteral("ZIP 条目损坏"); break;
        }
        QByteArray name(info.size_filename, '\0');
        if (unzGetCurrentFileInfo64(handle_, &info, name.data(), name.size(), nullptr, 0, nullptr, 0) != UNZ_OK || name.contains('\0')) {
            error_ = QStringLiteral("ZIP 文件名损坏"); break;
        }
        auto decoder = QStringDecoder(QStringDecoder::Utf8);
        QString decoded = decoder(name);
        if (decoder.hasError()) {
            if (info.flag & 0x800) { error_ = QStringLiteral("ZIP UTF-8 文件名损坏"); break; }
            decoded = QString::fromLocal8Bit(name);
        }
        if (positions_.contains(decoded)) { error_ = QStringLiteral("ZIP 存在重复条目：") + decoded; break; }
        ZipInfo entry;
        entry.filePath = decoded;
        const unsigned mode = info.external_fa >> 16;
        entry.isSymLink = (mode & 0170000) == 0120000;
        entry.isDir = decoded.endsWith(QChar('/')) || (mode & 0170000) == 0040000 || (info.external_fa & 0x10);
        entry.isFile = !entry.isDir && !entry.isSymLink;
        entry.size = qint64(info.uncompressed_size);
        entry.compression = info.compression_method; entry.attributes = info.external_fa;
        entry.lastModified = QDateTime(QDate(info.tmu_date.tm_year, info.tmu_date.tm_mon + 1, info.tmu_date.tm_mday), QTime(info.tmu_date.tm_hour, info.tmu_date.tm_min, info.tmu_date.tm_sec));
        files_.append(entry);
        positions_.insert(decoded, unzGetOffset64(handle_));
        status = unzGoToNextFile(handle_);
    }
    if (!error_.isEmpty()) { files_.clear(); positions_.clear(); }
}

ZipReader::~ZipReader() { if (handle_) unzClose(handle_); }

bool ZipReader::streamFile(const QString &name, QIODevice *output, const Cancellation &cancel, const Progress &progress) {
    if (!error_.isEmpty() || !positions_.contains(name) || !output || !output->isWritable()) return false;
    if (unzSetOffset64(handle_, positions_.value(name)) != UNZ_OK || unzOpenCurrentFile(handle_) != UNZ_OK) {
        error_ = QStringLiteral("ZIP 条目无法打开：") + name; return false;
    }
    QByteArray buffer(1024 * 1024, '\0');
    bool valid = true;
    qint64 total = 0;
    qint64 expected = 0;
    for (const ZipInfo &entry : files_) if (entry.filePath == name) { expected = entry.size; break; }
    while (true) {
        if (cancel && cancel->load()) { valid = false; error_ = QStringLiteral("操作已取消"); break; }
        const int count = unzReadCurrentFile(handle_, buffer.data(), buffer.size());
        if (count < 0 || (count > 0 && (total > expected - count || output->write(buffer.constData(), count) != count))) {
            valid = false; error_ = QStringLiteral("ZIP 解压或写入失败：") + name; break;
        }
        if (!count) break;
        total += count;
        if (progress) progress(total, expected, name);
    }
    if (unzCloseCurrentFile(handle_) != UNZ_OK || total != expected) { valid = false; if (error_.isEmpty()) error_ = QStringLiteral("ZIP CRC 或长度校验失败：") + name; }
    return valid;
}

QByteArray ZipReader::fileData(const QString &name, qint64 limit) {
    bool found = false;
    for (const ZipInfo &entry : files_) if (entry.filePath == name) {
        if (entry.size > limit || entry.size < 0 || !entry.isFile) return {};
        found = true; break;
    }
    if (!found) return {};
    QBuffer output; output.open(QIODevice::WriteOnly);
    if (!streamFile(name, &output)) return {};
    return output.data();
}

ZipWriter::ZipWriter(QIODevice *output) { open(output); }
ZipWriter::ZipWriter(const QString &path) : file_(std::make_unique<QFile>(path)) {
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) { failed_ = true; return; }
    open(file_.get());
}
void ZipWriter::open(QIODevice *output) {
    if (!output || !output->isWritable()) { failed_ = true; return; }
    auto functions = callbacks(output);
    output_ = output;
    handle_ = zipOpen2_64("qt", APPEND_STATUS_CREATE, nullptr, &functions);
    if (!handle_) failed_ = true;
}
ZipWriter::~ZipWriter() { close(); }
void ZipWriter::close() { if (handle_) { if (zipClose(handle_, nullptr) != ZIP_OK) failed_ = true; handle_ = nullptr; } }
void ZipWriter::add(const QString &name, QIODevice *input, unsigned attributes, const Cancellation &cancel, const ZipInfo *original, const Progress &progress) {
    if (!handle_ || failed_) { failed_ = true; return; }
    zip_fileinfo info{};
    info.external_fa = original ? original->attributes : attributes;
    const QDateTime now = original && original->lastModified.isValid() ? original->lastModified : QDateTime::currentDateTime();
    info.tmz_date = {now.time().second(), now.time().minute(), now.time().hour(), now.date().day(), now.date().month() - 1, now.date().year()};
    const QByteArray encoded = name.toUtf8();
    const bool large = input && input->size() >= 0xffffffffLL;
    const int method = original && original->compression == 0 ? 0 : Z_DEFLATED;
    QByteArray extra;
    if (input && method == 0) {
        const qint64 alignment = name.endsWith(QStringLiteral(".so")) ? 16384 : 4;
        const qint64 padding = (alignment - (output_->pos() + 30 + encoded.size() + (large ? 20 : 0) + 4) % alignment) % alignment;
        extra = QByteArray(4 + padding, '\0'); extra[0] = char(0x35); extra[1] = char(0xd9); extra[2] = char(padding & 255); extra[3] = char((padding >> 8) & 255);
    }
    if (zipOpenNewFileInZip4_64(handle_, encoded.constData(), &info, extra.constData(), uInt(extra.size()), nullptr, 0, nullptr,
        method, Z_DEFAULT_COMPRESSION, 0, -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY, nullptr, 0, (3 << 8) | 45, 0x800, large) != ZIP_OK) { failed_ = true; return; }
    qint64 done = 0;
    while (input && !input->atEnd()) {
        if (cancel && cancel->load()) { failed_ = true; break; }
        const QByteArray bytes = input->read(1024 * 1024);
        if (bytes.isEmpty() || zipWriteInFileInZip(handle_, bytes.constData(), bytes.size()) != ZIP_OK) { failed_ = true; break; }
        done += bytes.size(); if (progress) progress(done, input->size(), name);
    }
    if (zipCloseFileInZip(handle_) != ZIP_OK) failed_ = true;
}
void ZipWriter::addDirectory(const QString &name, const ZipInfo *original) { add(name.endsWith(QChar('/')) ? name : name + QChar('/'), nullptr, (0040755U << 16) | 0x10, {}, original); }
void ZipWriter::addSymLink(const QString &name, const QString &target) { QBuffer input; input.setData(target.toUtf8()); input.open(QIODevice::ReadOnly); add(name, &input, 0120777U << 16, {}); }
void ZipWriter::addFile(const QString &name, const QByteArray &bytes) { QBuffer input; input.setData(bytes); input.open(QIODevice::ReadOnly); addFile(name, &input); }
void ZipWriter::addFile(const QString &name, QIODevice *input, const Cancellation &cancel, const ZipInfo *original, const Progress &progress) { add(name, input, 0100644U << 16, cancel, original, progress); }

}
