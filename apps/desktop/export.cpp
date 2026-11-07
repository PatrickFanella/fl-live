#include "authored_project.hpp"
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

void exportProjectWav(const QString& path, const ProjectSettings& settings, const fl_live::DocumentSnapshot& identities, int bars) {
    if (bars < 1 || bars > 16) throw std::invalid_argument("Export length must be 1 to 16 bars");
    const fl_live::DocumentRenderPlan prepared(identities, templateSources(settings, identities), 48000,
        static_cast<std::uint32_t>(settings.tempo) * 1000U, {}, templateGains(settings, identities));
    fl_live::Engine engine(prepared.plan());
    for (std::size_t track = 0; track < prepared.plan().tracks.size(); ++track)
        if (engine.schedule({fl_live::CommandKind::launch, 0, track, 0}) != fl_live::ScheduleResult::accepted)
            throw std::runtime_error("Cannot schedule export clips");
    engine.start();
    const auto numerator = 48000ULL * 60 * 4 * static_cast<unsigned>(bars);
    const auto frames = (numerator + static_cast<unsigned>(settings.tempo) - 1) / static_cast<unsigned>(settings.tempo);
    constexpr quint16 channels = 2;
    const auto size = static_cast<quint32>(frames * 2 * channels);
    QByteArray header(44, '\0');
    auto u16 = [&](int at, quint16 value) { qToLittleEndian(value, header.data() + at); };
    auto u32 = [&](int at, quint32 value) { qToLittleEndian(value, header.data() + at); };
    header.replace(0, 4, "RIFF"); u32(4, size + 36); header.replace(8, 8, "WAVEfmt ");
    u32(16, 16); u16(20, 1); u16(22, channels); u32(24, 48000); u32(28, 48000U * 2 * channels);
    u16(32, 2 * channels); u16(34, 16); header.replace(36, 4, "data"); u32(40, size);
    QFile file(path);
    // Exclusive creation preserves every existing destination, including links.
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create WAV: choose a new writable file path");
    try {
        if (file.write(header) != header.size()) throw std::runtime_error("Cannot write WAV header");
        std::array<float, 257 * channels> audio{};
        std::array<char, 257 * channels * 2> pcm{};
        for (quint64 offset = 0; offset < frames;) {
            const auto count = static_cast<std::size_t>(std::min<quint64>(257, frames - offset)) * channels;
            if (!engine.process(std::span<float>(audio.data(), count), channels)) throw std::runtime_error("Export render refused");
            for (std::size_t i = 0; i < count; ++i) {
                const auto value = static_cast<qint16>(std::lround(std::clamp(audio[i], -1.0F, 1.0F) * 32767.0F));
                qToLittleEndian(value, pcm.data() + i * 2);
            }
            const auto bytes = static_cast<qint64>(count * 2);
            if (file.write(pcm.data(), bytes) != bytes) throw std::runtime_error("Cannot write WAV samples");
            offset += count / channels;
        }
        if (!file.flush()) throw std::runtime_error("Cannot flush WAV");
    } catch (...) {
        file.remove(); // Only the newly created export; never a pre-existing destination.
        throw;
    }
}
