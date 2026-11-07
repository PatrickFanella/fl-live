#include "authored_project.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

std::uint64_t exportFrames(int tempo, int bars) {
    const auto numerator = 48000ULL * 60 * 4 * static_cast<unsigned>(bars);
    return (numerator + static_cast<unsigned>(tempo) - 1) / static_cast<unsigned>(tempo);
}
QString exportPartialPath(const QString& path) { return path + QStringLiteral(".part"); }

ExportOutcome exportProjectWav(const QString& path, const SessionContent& content, const fl_live::DocumentSnapshot& identities,
    int bars, ExportProgress* progress) {
    if (bars < 1 || bars > 16) throw std::invalid_argument("Export length must be 1 to 16 bars");
    if (path.trimmed().isEmpty()) throw std::invalid_argument("Enter a new WAV file path");
    // Exclusive checks preserve every existing destination, including links.
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink())
        throw std::runtime_error("Cannot create WAV: choose a new writable file path");
    const fl_live::DocumentRenderPlan prepared(identities, contentSources(content, identities), 48000,
        static_cast<std::uint32_t>(content.tempo) * 1000U, {}, contentGains(content, identities));
    fl_live::Engine engine(prepared.plan());
    for (std::size_t track = 0; track < prepared.plan().tracks.size(); ++track)
        if (engine.schedule({fl_live::CommandKind::launch, 0, track, 0}) != fl_live::ScheduleResult::accepted)
            throw std::runtime_error("Cannot schedule export clips");
    engine.start();
    const auto frames = exportFrames(content.tempo, bars);
    if (progress) progress->total.store(frames);
    constexpr quint16 channels = 2;
    const auto size = static_cast<quint32>(frames * 2 * channels);
    QByteArray header(44, '\0');
    auto u16 = [&](int at, quint16 value) { qToLittleEndian(value, header.data() + at); };
    auto u32 = [&](int at, quint32 value) { qToLittleEndian(value, header.data() + at); };
    header.replace(0, 4, "RIFF"); u32(4, size + 36); header.replace(8, 8, "WAVEfmt ");
    u32(16, 16); u16(20, 1); u16(22, channels); u32(24, 48000); u32(28, 48000U * 2 * channels);
    u16(32, 2 * channels); u16(34, 16); header.replace(36, 4, "data"); u32(40, size);
    // Render into NAME.part, then publish with link(), which never replaces an
    // existing destination. Success, cancellation and failure all remove the
    // partial file; only a crash can leave it behind.
    const auto partial = exportPartialPath(path);
    QFile file(partial);
    if (QFileInfo::exists(partial) || QFileInfo(partial).isSymLink())
        throw std::runtime_error("An earlier export to this path was interrupted. Delete " +
            QFileInfo(partial).fileName().toStdString() + " or choose another path");
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create WAV: choose a new writable file path");
    try {
        if (file.write(header) != header.size()) throw std::runtime_error("Cannot write WAV header");
        std::array<float, 257 * channels> audio{};
        std::array<char, 257 * channels * 2> pcm{};
        for (quint64 offset = 0; offset < frames;) {
            if (progress && progress->cancel.load(std::memory_order_relaxed)) {
                file.close();
                QFile::remove(partial);
                return ExportOutcome::cancelled;
            }
            const auto count = static_cast<std::size_t>(std::min<quint64>(257, frames - offset)) * channels;
            if (!engine.process(std::span<float>(audio.data(), count), channels)) throw std::runtime_error("Export render refused");
            for (std::size_t i = 0; i < count; ++i) {
                const auto value = static_cast<qint16>(std::lround(std::clamp(audio[i], -1.0F, 1.0F) * 32767.0F));
                qToLittleEndian(value, pcm.data() + i * 2);
            }
            const auto bytes = static_cast<qint64>(count * 2);
            if (file.write(pcm.data(), bytes) != bytes) throw std::runtime_error("Cannot write WAV samples; the partial export was removed");
            offset += count / channels;
            if (progress) progress->frames.store(offset, std::memory_order_relaxed);
        }
        if (!file.flush() || ::fdatasync(file.handle()) != 0) throw std::runtime_error("Cannot flush WAV; the partial export was removed");
        file.close();
        if (::link(QFile::encodeName(partial).constData(), QFile::encodeName(path).constData()) != 0)
            throw std::runtime_error(errno == EEXIST ? "Another file appeared at the export path; nothing was replaced"
                                                     : "Cannot publish the WAV file; the partial export was removed");
        QFile::remove(partial);
        const auto directory = QFile::encodeName(QFileInfo(path).absolutePath());
        if (const int fd = ::open(directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); fd >= 0) { ::fsync(fd); ::close(fd); }
        return ExportOutcome::completed;
    } catch (...) {
        file.close();
        QFile::remove(partial); // Only the partial file this export created.
        throw;
    }
}
