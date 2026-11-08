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
    ExportOptions options;
    options.bars = bars;
    if (bars < 1 || bars > 16) throw std::invalid_argument("Export length must be 1 to 16 bars");
    return exportProjectWav(path, content, identities, options, progress);
}
void validateExportOptions(const ExportOptions& o) {
    if (o.bars < 1 || o.bars > ExportOptions::bar_limit) throw std::invalid_argument("Export length must be 1 to 64 bars");
    if (o.start_bar < 0 || o.start_bar > ExportOptions::start_limit) throw std::invalid_argument("Export start must be bar 1 to 256");
    if (o.tail_ms < 0 || o.tail_ms > ExportOptions::tail_limit_ms) throw std::invalid_argument("Export tail must be 0 to 10000 ms");
    if (o.channels != 1 && o.channels != 2) throw std::invalid_argument("Export channels must be mono or stereo");
    if (o.format != ExportFormat::pcm16 && o.format != ExportFormat::pcm24 && o.format != ExportFormat::float32)
        throw std::invalid_argument("Unsupported export format");
}
ExportOutcome exportProjectWav(const QString& path, const SessionContent& content, const fl_live::DocumentSnapshot& identities,
    const ExportOptions& options, ExportProgress* progress) {
    validateExportOptions(options);
    if (path.trimmed().isEmpty()) throw std::invalid_argument("Enter a new WAV file path");
    // Exclusive checks preserve every existing destination, including links.
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink())
        throw std::runtime_error("Cannot create WAV: " + QFileInfo(path).fileName().toStdString() + " already exists; choose a new file name");
    if (!QFileInfo(QFileInfo(path).absolutePath()).isDir())
        throw std::runtime_error("Cannot create WAV: the folder " + QFileInfo(path).absolutePath().toStdString() + " does not exist");
    const fl_live::DocumentRenderPlan prepared(identities, contentSources(content, identities), 48000,
        static_cast<std::uint32_t>(content.tempo) * 1000U, {}, contentGains(content, identities), contentAutomation(content, identities));
    fl_live::Engine engine(prepared.plan());
    // Every clip starts at the song start (frame 0). Frames before the range
    // are rendered and discarded, so the range matches playback from the top.
    // At the range end every track stops; the tail is rendered after that.
    const auto first = exportFrames(content.tempo, options.start_bar);
    const auto end = exportFrames(content.tempo, options.start_bar + options.bars);
    const auto tail = static_cast<std::uint64_t>(options.tail_ms) * 48U;
    std::array<fl_live::Command, fl_live::Engine::track_capacity> launch{}, halt{};
    const auto tracks = prepared.plan().tracks.size();
    for (std::size_t track = 0; track < tracks; ++track) {
        const auto clip = track < options.clips.size() ? options.clips[track] : 0;
        if (clip >= prepared.plan().tracks[track].clips.size()) throw std::runtime_error("Export clip does not exist");
        launch[track] = {fl_live::CommandKind::launch, 0, track, clip};
        halt[track] = {fl_live::CommandKind::stop_track, end, track, 0};
    }
    if (tracks && (engine.scheduleTogether(std::span(launch).first(tracks)) != fl_live::ScheduleResult::accepted ||
                   engine.scheduleTogether(std::span(halt).first(tracks)) != fl_live::ScheduleResult::accepted))
        throw std::runtime_error("Cannot schedule export clips");
    engine.start();
    const auto frames = end - first + tail;
    if (progress) progress->total.store(end + tail);
    const quint16 channels = static_cast<quint16>(options.channels);
    const quint16 bytesPerSample = options.format == ExportFormat::pcm16 ? 2 : options.format == ExportFormat::pcm24 ? 3 : 4;
    const auto dataBytes = frames * channels * bytesPerSample;
    if (dataBytes > 0xFFFFFFFFULL - 36) throw std::invalid_argument("Export is larger than a WAV file can hold; shorten it");
    const auto size = static_cast<quint32>(dataBytes);
    QByteArray header(44, '\0');
    auto u16 = [&](int at, quint16 value) { qToLittleEndian(value, header.data() + at); };
    auto u32 = [&](int at, quint32 value) { qToLittleEndian(value, header.data() + at); };
    header.replace(0, 4, "RIFF"); u32(4, size + 36); header.replace(8, 8, "WAVEfmt ");
    u32(16, 16); u16(20, options.format == ExportFormat::float32 ? 3 : 1); u16(22, channels); u32(24, 48000);
    u32(28, 48000U * channels * bytesPerSample); u16(32, static_cast<quint16>(channels * bytesPerSample));
    u16(34, static_cast<quint16>(bytesPerSample * 8)); header.replace(36, 4, "data"); u32(40, size);
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
        std::array<float, 257 * 2> audio{};
        std::array<char, 257 * 2 * 4> encoded{};
        for (quint64 position = 0; position < end + tail;) {
            if (progress && progress->cancel.load(std::memory_order_relaxed)) {
                file.close();
                QFile::remove(partial);
                return ExportOutcome::cancelled;
            }
            const auto count = static_cast<std::size_t>(std::min<quint64>(257, end + tail - position));
            if (!engine.process(std::span<float>(audio.data(), count * channels), channels)) throw std::runtime_error("Export render refused");
            // Write only the part of this block inside [first, end + tail).
            const auto skip = position < first ? static_cast<std::size_t>(std::min<quint64>(count, first - position)) : 0;
            std::size_t at = 0;
            for (std::size_t i = skip * channels; i < count * channels; ++i) {
                const float v = audio[i];
                if (options.format == ExportFormat::pcm16) {
                    qToLittleEndian(static_cast<qint16>(std::lround(std::clamp(v, -1.0F, 1.0F) * 32767.0F)), encoded.data() + at); at += 2;
                } else if (options.format == ExportFormat::pcm24) {
                    const auto n = static_cast<qint32>(std::lround(std::clamp(v, -1.0F, 1.0F) * 8388607.0F));
                    encoded[at++] = static_cast<char>(n & 0xff); encoded[at++] = static_cast<char>((n >> 8) & 0xff); encoded[at++] = static_cast<char>((n >> 16) & 0xff);
                } else { qToLittleEndian(v, encoded.data() + at); at += 4; } // unclipped, as mixed
            }
            if (at && file.write(encoded.data(), static_cast<qint64>(at)) != static_cast<qint64>(at))
                throw std::runtime_error("Cannot write WAV samples; the partial export was removed");
            position += count;
            if (progress) progress->frames.store(position, std::memory_order_relaxed);
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
