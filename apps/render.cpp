#include "fl_live/engine.hpp"
#include <sndfile.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
fl_live::SessionPlan demo() {
    using namespace fl_live;
    SessionPlan plan;
    AudioClip pulse;
    pulse.samples.resize(24000, 0);
    for (std::size_t i = 0; i < 2400; ++i)
        pulse.samples[i] = static_cast<float>(0.3 * std::exp(-static_cast<double>(i) / 480.0) *
            std::sin(2.0 * std::numbers::pi * 90.0 * static_cast<double>(i) / 48000.0));
    MidiClip bass{96000, {{0, 18000, 36, 0.8F}, {24000, 18000, 43, 0.6F},
        {48000, 18000, 39, 0.8F}, {72000, 18000, 46, 0.6F}}, true};
    plan.tracks.push_back(Track{0.8F, {std::move(pulse)}});
    plan.tracks.push_back(Track{1.0F, {std::move(bass)}});
    return plan;
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 3) {
            std::cerr << "Usage: fl-live-render NEW_OUTPUT.wav [block-size:1..8192]\n";
            return 2;
        }
        std::size_t block = 257;
        if (argc == 3) {
            const std::string text(argv[2]);
            const auto result = std::from_chars(text.data(), text.data() + text.size(), block);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || block == 0 || block > 8192)
                throw std::invalid_argument("block size must be an integer in 1..8192");
        }
        const auto plan = demo();
        fl_live::Engine engine(plan);
        const auto boundary = fl_live::TempoGrid(plan.sample_rate, plan.bpm_milli).next_bar(1);
        for (const auto command : {
                fl_live::Command{fl_live::CommandKind::launch, boundary, 0, 0},
                fl_live::Command{fl_live::CommandKind::launch, boundary, 1, 0},
                fl_live::Command{fl_live::CommandKind::stop_track, 288000, 0},
                fl_live::Command{fl_live::CommandKind::stop_track, 288000, 1}})
            if (engine.schedule(command) != fl_live::ScheduleResult::accepted)
                throw std::runtime_error("demo command rejected");

        // File I/O is exclusively in this offline driver, never Engine::process.
        SF_INFO info{};
        info.samplerate = static_cast<int>(plan.sample_rate);
        info.channels = 1;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        const int fd = ::open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) throw std::runtime_error("cannot create output (must be a new writable path)");
        SNDFILE* raw = sf_open_fd(fd, SFM_WRITE, &info, SF_TRUE);
        if (!raw) { ::close(fd); throw std::runtime_error(sf_strerror(nullptr)); }
        std::unique_ptr<SNDFILE, decltype(&sf_close)> file(raw, sf_close);
        const int clipping = SF_TRUE;
        sf_command(file.get(), SFC_SET_CLIPPING, nullptr, clipping);
        std::vector<float> buffer(block);
        engine.start();
        constexpr fl_live::Frame duration = 384000;
        while (engine.position() < duration) {
            const auto frames = static_cast<std::size_t>(std::min<fl_live::Frame>(block, duration - engine.position()));
            engine.process(std::span(buffer).first(frames));
            if (sf_writef_float(file.get(), buffer.data(), static_cast<sf_count_t>(frames)) != static_cast<sf_count_t>(frames))
                throw std::runtime_error("short WAV write");
        }
        engine.stop();
        if (sf_close(file.release()) != 0) throw std::runtime_error("WAV close failed");
        std::cout << "Rendered " << duration << " mono frames at " << plan.sample_rate
                  << " Hz; launch=" << boundary << "; stop=288000; block=" << block << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fl-live-render: " << error.what() << '\n';
        return 1;
    }
}
