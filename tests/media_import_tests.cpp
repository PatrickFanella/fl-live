#include "fl_live/media_import.hpp"
#include "fl_live/document_render.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

// Fixtures are written by an independent RIFF writer below, not by libsndfile,
// so the importer is checked against bytes it did not produce.
namespace {
using namespace fl_live;
namespace fs = std::filesystem;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void put16(std::string& out, std::uint16_t v) { out.push_back(char(v & 0xff)); out.push_back(char(v >> 8)); }
void put32(std::string& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xff)); }
// format 1 = PCM, 3 = IEEE float. data is already little-endian sample bytes.
std::string wav(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits, const std::string& data) {
    std::string out = "RIFF";
    put32(out, static_cast<std::uint32_t>(36 + data.size()));
    out += "WAVEfmt ";
    put32(out, 16); put16(out, format); put16(out, channels); put32(out, rate);
    put32(out, rate * channels * bits / 8U); put16(out, static_cast<std::uint16_t>(channels * bits / 8U)); put16(out, bits);
    out += "data";
    put32(out, static_cast<std::uint32_t>(data.size()));
    return out + data;
}
std::string pcm16(std::initializer_list<int> values) {
    std::string out;
    for (int v : values) put16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
    return out;
}
std::string pcm24(std::initializer_list<int> values) {
    std::string out;
    for (int v : values) { const auto u = static_cast<std::uint32_t>(v); out.push_back(char(u & 0xff)); out.push_back(char((u >> 8) & 0xff)); out.push_back(char((u >> 16) & 0xff)); }
    return out;
}
std::string f32(std::initializer_list<float> values) {
    std::string out;
    for (float v : values) { std::uint32_t u; std::memcpy(&u, &v, 4); put32(out, u); }
    return out;
}
struct Fixtures {
    fs::path root = fs::temp_directory_path() / ("fl-live-import-" + std::to_string(::getpid()));
    Fixtures() { fs::create_directories(root); }
    ~Fixtures() { std::error_code ignored; fs::remove_all(root, ignored); }
    fs::path write(const std::string& name, const std::string& bytes) {
        const auto path = root / name;
        std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    }
};
void expect(const WavImport& result, ImportError error, const char* what) {
    require(result.error == error, std::string(what) + ": unexpected import result " + std::to_string(static_cast<int>(result.error)) + " " + result.message);
    require(error == ImportError::none ? result.message.empty() : !result.message.empty(), std::string(what) + ": message policy");
}
void supported(Fixtures& f) {
    const auto mono = importWav(f.write("mono16.wav", wav(1, 1, 48000, 16, pcm16({0, 16384, -16384, 32767, -32768}))));
    expect(mono, ImportError::none, "mono PCM16");
    require(mono.channels == 1 && mono.frames == 5 && mono.source_rate == 48000 && mono.clip.channels == 1, "mono metadata");
    require(mono.clip.samples == std::vector<float>{0, 0.5F, -0.5F, 32767.0F / 32768, -1}, "PCM16 normalization");
    const auto stereo = importWav(f.write("stereo16.wav", wav(1, 2, 48000, 16, pcm16({8192, -8192, 16384, -16384}))));
    expect(stereo, ImportError::none, "stereo PCM16");
    require(stereo.channels == 2 && stereo.frames == 2 && stereo.clip.channels == 2, "stereo metadata");
    require(stereo.clip.samples == std::vector<float>{0.25F, -0.25F, 0.5F, -0.5F}, "stereo interleaving");
    const auto deep = importWav(f.write("mono24.wav", wav(1, 1, 48000, 24, pcm24({4194304, -4194304, 1}))));
    expect(deep, ImportError::none, "PCM24");
    require(deep.clip.samples[0] == 0.5F && deep.clip.samples[1] == -0.5F && deep.clip.samples[2] == 1.0F / 8388608, "PCM24 normalization");
    const auto floating = importWav(f.write("float.wav", wav(3, 2, 48000, 32, f32({0.125F, 1.5F, -2.0F, 0}))));
    expect(floating, ImportError::none, "float32");
    require(floating.clip.samples == std::vector<float>{0.125F, 1.5F, -2.0F, 0}, "float values kept as stored, unclipped");
    // A different session rate is admitted when the file matches it.
    WavImportPolicy rate44{44100};
    expect(importWav(f.write("rate44.wav", wav(1, 1, 44100, 16, pcm16({1, 2}))), rate44), ImportError::none, "matching 44.1 kHz");
    // Imported clips prepare and render through the engine as authored.
    DocumentSnapshot document;
    document.next_id = 4;
    document.tracks = {{TrackId{1}, "Imported", TrackKind::audio}};
    document.clips = {{ClipId{2}, TrackId{1}, "stereo16"}};
    const std::array sources{DocumentClipSource{ClipId{2}, stereo.clip}};
    const DocumentRenderPlan plan(document, sources);
    Engine engine(plan.plan());
    require(engine.schedule(*plan.launch(ClipId{2}, 0)) == ScheduleResult::accepted, "imported clip launch");
    engine.start();
    std::array<float, 8> out{};
    require(engine.process(out, 2), "stereo render");
    require(out == std::array<float, 8>{0.25F, -0.25F, 0.5F, -0.5F, 0.25F, -0.25F, 0.5F, -0.5F}, "imported clip render");
}
void refused(Fixtures& f) {
    expect(importWav(f.root / "missing.wav"), ImportError::cannot_open, "missing file");
    expect(importWav(f.root), ImportError::cannot_open, "directory");
    expect(importWav(f.write("text.wav", "not audio at all, just text")), ImportError::not_wav, "garbage");
    expect(importWav(f.write("truncated.wav", wav(1, 1, 48000, 16, pcm16({1, 2, 3})).substr(0, 20))), ImportError::not_wav, "truncated header");
    // A valid AIFF header: real audio, wrong container.
    std::string aiff = "FORM";
    const auto be32 = [&](std::uint32_t v) { for (int i = 3; i >= 0; --i) aiff.push_back(char((v >> (8 * i)) & 0xff)); };
    be32(50); aiff += "AIFFCOMM"; be32(18); aiff += std::string{0, 1}; be32(2); aiff += std::string{0, 16};
    aiff += std::string{'\x40', '\x0e', '\xbb', '\x80', 0, 0, 0, 0, 0, 0}; // 48000 as 80-bit extended
    aiff += "SSND"; be32(12); be32(0); be32(0); aiff += std::string{0, 1, 0, 2};
    expect(importWav(f.write("audio.aiff", aiff)), ImportError::not_wav, "AIFF container");
    expect(importWav(f.write("alaw.wav", wav(6, 1, 48000, 8, std::string{'\x55', '\xd5'}))), ImportError::unsupported_encoding, "A-law");
    expect(importWav(f.write("three.wav", wav(1, 3, 48000, 16, pcm16({1, 2, 3})))), ImportError::unsupported_channels, "three channels");
    const auto rate = importWav(f.write("rate44-default.wav", wav(1, 1, 44100, 16, pcm16({1, 2}))));
    expect(rate, ImportError::sample_rate_mismatch, "rate mismatch");
    require(rate.message.find("44100 Hz") != std::string::npos && rate.message.find("48000 Hz") != std::string::npos, "rate message names both rates");
    expect(importWav(f.write("empty.wav", wav(1, 1, 48000, 16, ""))), ImportError::empty, "empty data");
    expect(importWav(f.write("nan.wav", wav(3, 1, 48000, 32, f32({0, std::numeric_limits<float>::quiet_NaN()})))), ImportError::non_finite, "NaN");
    expect(importWav(f.write("inf.wav", wav(3, 1, 48000, 32, f32({std::numeric_limits<float>::infinity()})))), ImportError::non_finite, "infinity");
    WavImportPolicy short_limit;
    short_limit.frame_limit = 4;
    expect(importWav(f.write("five.wav", wav(1, 1, 48000, 16, pcm16({1, 2, 3, 4, 5}))), short_limit), ImportError::too_long, "frame limit");
    expect(importWav(f.root / "five.wav", WavImportPolicy{48000, 5}), ImportError::none, "frame limit is inclusive");
    // Header claims fewer frames than present: the read loop still enforces the limit.
    auto lying = wav(1, 1, 48000, 16, pcm16({1, 2, 3, 4, 5, 6}));
    lying[40] = 4; // data chunk claims two frames
    expect(importWav(f.write("lying.wav", lying), WavImportPolicy{48000, 6}), ImportError::none, "short header is safe");
    expect(importWav(f.root / "five.wav", WavImportPolicy{48000, 0}), ImportError::invalid_policy, "zero limit");
    expect(importWav(f.root / "five.wav", WavImportPolicy{1000}), ImportError::invalid_policy, "bad session rate");
    std::atomic<bool> cancelled{true};
    expect(importWav(f.root / "five.wav", {}, &cancelled), ImportError::cancelled, "pre-cancelled");
}
void worker(Fixtures& f) {
    // Large enough that decoding takes several chunks.
    std::string data;
    for (int i = 0; i < 48000 * 20; ++i) put16(data, static_cast<std::uint16_t>(i));
    const auto big = f.write("big.wav", wav(1, 1, 48000, 16, data));
    const auto good = f.write("good.wav", wav(1, 1, 48000, 16, pcm16({16384})));
    const auto bad = f.write("bad.wav", wav(1, 1, 22050, 16, pcm16({1})));
    WavImportWorker worker;
    require(worker.worker_id() != std::this_thread::get_id(), "decoding must run on a separate worker thread");
    const auto first = worker.submit(big), second = worker.submit(good), third = worker.submit(bad), fourth = worker.submit(good);
    worker.cancel(fourth);
    std::vector<WavImportWorker::Result> results;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (results.size() < 4 && std::chrono::steady_clock::now() < deadline) {
        if (auto result = worker.poll()) results.push_back(std::move(*result));
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(results.size() == 4, "worker did not finish");
    const auto find = [&](std::uint64_t ticket) -> const WavImport& {
        for (const auto& r : results) if (r.ticket == ticket) return r.import;
        throw std::runtime_error("missing ticket");
    };
    expect(find(fourth), ImportError::cancelled, "pending cancel");
    expect(find(first), ImportError::none, "big import");
    require(find(first).frames == 48000U * 20, "big import length");
    expect(find(second), ImportError::none, "queued import");
    expect(find(third), ImportError::sample_rate_mismatch, "queued failure");
    // Destruction while decoding cancels and joins promptly.
    const auto started = std::chrono::steady_clock::now();
    {
        WavImportWorker interrupted;
        (void)interrupted.submit(big);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "worker shutdown must not wait for a full decode");
    WavImportWorker running;
    const auto ticket = running.submit(big);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    running.cancel(ticket);
    std::optional<WavImportWorker::Result> result;
    while (!result) { result = running.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    require(result->ticket == ticket && (result->import.error == ImportError::cancelled || result->import.error == ImportError::none),
            "running cancel must finish as cancelled (or complete if already done)");
}
}
int main() {
    try {
        Fixtures fixtures;
        supported(fixtures); refused(fixtures); worker(fixtures);
        std::cout << "PASS: WAV import policy, actionable refusals, worker order/cancel/shutdown\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
