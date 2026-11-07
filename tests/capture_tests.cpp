// Capture pipeline (#44): the callback side neither allocates nor performs
// file I/O, the worker writes every accepted block in order, overflow drops
// whole blocks and is counted, and a write failure keeps what was written.
// File calls are interposed here so the test can see which thread makes them
// and inject a failing write.
#include "fl_live/capture.hpp"
#include <sndfile.h>
#include <dlfcn.h>
#include <csignal>
#include <fstream>
#include <iterator>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local bool in_callback = false;
std::atomic<int> callback_io{0}, callback_allocations{0};
std::atomic<bool> fail_writes{false};
template<class F> F real(const char* name) { return reinterpret_cast<F>(dlsym(RTLD_NEXT, name)); }
}
extern "C" {
ssize_t write(int fd, const void* data, size_t size) {
    static const auto next = real<ssize_t (*)(int, const void*, size_t)>("write");
    if (in_callback) callback_io.fetch_add(1);
    if (fail_writes.load() && fd > 2) { errno = ENOSPC; return -1; }
    return next(fd, data, size);
}
ssize_t pwrite(int fd, const void* data, size_t size, off_t at) {
    static const auto next = real<ssize_t (*)(int, const void*, size_t, off_t)>("pwrite");
    if (in_callback) callback_io.fetch_add(1);
    if (fail_writes.load() && fd > 2) { errno = ENOSPC; return -1; }
    return next(fd, data, size, at);
}
int open(const char* path, int flags, ...) {
    static const auto next = real<int (*)(const char*, int, ...)>("open");
    if (in_callback) callback_io.fetch_add(1);
    va_list args; va_start(args, flags); const auto mode = va_arg(args, int); va_end(args);
    return next(path, flags, mode);
}
int fsync(int fd) {
    static const auto next = real<int (*)(int)>("fsync");
    if (in_callback) callback_io.fetch_add(1);
    return next(fd);
}
}
void* operator new(std::size_t size) {
    if (in_callback) callback_allocations.fetch_add(1);
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace fl_live;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<float> readWav(const std::filesystem::path& path, int& channels) {
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    require(file != nullptr, "recorded WAV opens");
    channels = info.channels;
    std::vector<float> samples(static_cast<std::size_t>(info.frames * info.channels));
    require(sf_readf_float(file, samples.data(), info.frames) == info.frames, "recorded WAV reads");
    sf_close(file);
    require((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV && (info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_FLOAT && info.samplerate == 48000,
        "32-bit float WAV at 48 kHz");
    return samples;
}
// Block b of 257 stereo frames carries values b*1000 + frame on the left and
// the negated value on the right, so any block is identifiable in the file.
std::vector<float> block(int b) {
    std::vector<float> samples(2 * 257);
    for (int f = 0; f < 257; ++f) { samples[2 * f] = static_cast<float>(b * 1000 + f) / 1e6F; samples[2 * f + 1] = -samples[2 * f]; }
    return samples;
}

void ringContract() {
    CaptureRing ring(10);
    const std::vector<float> six{1, 2, 3, 4, 5, 6}, five{7, 8, 9, 10, 11};
    require(ring.push(six) && !ring.push(five), "all or nothing: five more do not fit in ten");
    std::vector<float> out(4);
    require(ring.pop(out) == 4 && out == std::vector<float>{1, 2, 3, 4}, "pop in order");
    require(ring.push(five), "fits after the pop, wrapping");
    std::vector<float> rest(10);
    require(ring.pop(rest) == 7 && std::vector<float>(rest.begin(), rest.begin() + 7) == std::vector<float>{5, 6, 7, 8, 9, 10, 11}, "wrapped order");
    require(ring.pop(rest) == 0, "empty");
}

void contentAndCallbackDiscipline(const std::filesystem::path& dir) {
    const auto path = dir / "take.wav";
    std::vector<std::vector<float>> blocks;
    for (int b = 0; b < 400; ++b) blocks.push_back(block(b));
    {
        CaptureTake take(path, 2, 48000, 48000);
        std::thread callback([&] {
            in_callback = true;
            for (const auto& b : blocks) { take.capture(b); std::this_thread::sleep_for(std::chrono::microseconds(500)); }
            in_callback = false;
        });
        callback.join();
        const auto status = take.finish();
        require(status.finished && !status.failed && !status.overflowed && status.frames_written == 400 * 257 && status.file == path,
            "every block written, renamed to the final name");
        require(!std::filesystem::exists(dir / "take.wav.part"), "no .part file left");
    }
    require(callback_io.load() == 0 && callback_allocations.load() == 0, "the capture call neither allocates nor touches files");
    int channels = 0;
    const auto samples = readWav(path, channels);
    std::vector<float> expected;
    for (const auto& b : blocks) expected.insert(expected.end(), b.begin(), b.end());
    require(channels == 2 && samples == expected, "the file holds every block in order, bit for bit");
    bool refused = false;
    try { CaptureTake again(path, 2, 48000); } catch (const std::invalid_argument&) { refused = true; }
    require(refused, "a take never replaces an existing file");
}

void overflowAccounting(const std::filesystem::path& dir) {
    const auto path = dir / "overflow.wav";
    CaptureTake take(path, 2, 48000, 600); // room for two blocks
    for (int b = 0; b < 200; ++b) take.capture(block(b)); // far faster than the worker
    const auto status = take.finish();
    require(status.overflowed && status.blocks_dropped > 0, "overflow is reported");
    require(status.frames_written + status.frames_dropped == 200 * 257, "every frame is either written or counted as dropped");
    int channels = 0;
    const auto samples = readWav(path, channels);
    require(samples.size() == status.frames_written * 2, "file length matches the written count");
    // The file is whole blocks in increasing order: dropped blocks leave no partial data.
    int previous = -1;
    for (std::size_t at = 0; at < samples.size(); at += 2 * 257) {
        const int b = static_cast<int>(std::lround(samples[at] * 1e6F)) / 1000;
        const auto expected = block(b);
        require(b > previous && std::equal(expected.begin(), expected.end(), samples.begin() + static_cast<std::ptrdiff_t>(at)), "whole blocks in order");
        previous = b;
    }
}

void writeFailure(const std::filesystem::path& dir) {
    const auto path = dir / "failed.wav";
    CaptureTake take(path, 2, 48000, 48000);
    for (int b = 0; b < 20; ++b) take.capture(block(b));
    for (int i = 0; i < 500 && take.status().frames_written < 20 * 257; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(take.status().frames_written == 20 * 257, "the first blocks are written");
    fail_writes.store(true);
    for (int b = 20; b < 40; ++b) take.capture(block(b));
    for (int i = 0; i < 500 && !take.status().failed; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto status = take.status();
    require(status.failed && status.error.find("failed.wav.part") != std::string::npos, "the failure is reported with the kept file");
    for (int b = 40; b < 60; ++b) take.capture(block(b)); // capture keeps returning
    fail_writes.store(false);
    status = take.finish();
    require(status.failed && status.file == dir / "failed.wav.part" && !std::filesystem::exists(path), "the .part file is kept, not renamed");
    int channels = 0;
    const auto samples = readWav(status.file, channels);
    require(samples.size() >= 20 * 257 * 2 && samples.size() == status.frames_written * 2, "the frames written before the failure are kept and readable");
    for (int b = 0; b < 20; ++b) {
        const auto expected = block(b);
        require(std::equal(expected.begin(), expected.end(), samples.begin() + b * 2 * 257), "kept frames are intact");
    }
}

std::string bytes(const std::filesystem::path& path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
void writeBytes(const std::filesystem::path& path, const std::string& data) { std::ofstream(path, std::ios::binary) << data; }

// #48: a process killed while recording leaves a .part file whose header
// never got its data size. Recovery copies every whole frame into a new
// file, checked here against the independently generated blocks.
void interruptedRecovery(const std::filesystem::path& dir) {
    const auto path = dir / "killed.wav", part = dir / "killed.wav.part";
    const pid_t child = fork();
    if (child == 0) {
        CaptureTake take(path, 2, 48000, 48000);
        for (int b = 0; b < 100; ++b) take.capture(block(b));
        for (int i = 0; i < 2000 && take.status().frames_written < 100 * 257; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ::kill(::getpid(), SIGKILL); // no finish(), no destructor, no header update
        std::_Exit(3);
    }
    int wstatus = 0;
    require(child > 0 && ::waitpid(child, &wstatus, 0) == child && WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL, "the recording process was killed");
    require(std::filesystem::exists(part) && !std::filesystem::exists(path), "the kill left only the .part file");
    const auto original = bytes(part);
    {
        // The header's data size was never written: the test proves nothing otherwise.
        const auto at = original.find("data");
        const auto* p = reinterpret_cast<const unsigned char*>(original.data() + at + 4);
        const std::uint32_t declared = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        std::cerr << "interrupted header declares " << declared << " data bytes for " << original.size() - at - 8 << " on disk\n";
        require(at != std::string::npos && declared != original.size() - at - 8, "the interrupted header does not describe the frames");
    }
    std::vector<float> expected;
    for (int b = 0; b < 100; ++b) { const auto x = block(b); expected.insert(expected.end(), x.begin(), x.end()); }
    const auto recovered = recoverTake(part, dir / "killed recovered.wav");
    require(recovered.recovered && recovered.frames == 100 * 257 && recovered.channels == 2 && recovered.rate == 48000 && recovered.discarded_bytes == 0,
        "every frame written before the kill is recovered");
    int channels = 0;
    require(readWav(recovered.file, channels) == expected && channels == 2, "the recovered file is the captured blocks bit for bit");
    require(bytes(part) == original, "the .part file is unchanged");
    const auto finished = bytes(recovered.file);
    const auto again = recoverTake(part, recovered.file);
    require(!again.recovered && again.message.find("never replaced") != std::string::npos && bytes(recovered.file) == finished, "an existing file is never replaced");
    require(!std::filesystem::exists(dir / "killed recovered.wav.recovering"), "no temporary file is left");

    // A file cut mid-frame keeps the whole frames and reports the remainder.
    const auto data = original.find("data") + 8;
    writeBytes(dir / "cut.wav.part", original.substr(0, data + 1000 * 8 + 3));
    const auto cut = recoverTake(dir / "cut.wav.part", dir / "cut.wav");
    require(cut.recovered && cut.frames == 1000 && cut.discarded_bytes == 3, "whole frames of a truncated file");
    require(readWav(cut.file, channels) == std::vector<float>(expected.begin(), expected.begin() + 2000), "truncated content intact");

    // Unrecoverable files are refused with a reason and left alone.
    const auto refused = [&](const std::string& name, const std::string& content, const char* reason) {
        writeBytes(dir / name, content);
        const auto r = recoverTake(dir / name, dir / (name + ".wav"));
        require(!r.recovered && r.message.find(reason) != std::string::npos && !std::filesystem::exists(dir / (name + ".wav")) && bytes(dir / name) == content, reason);
    };
    refused("header.part", original.substr(0, 30), "header is incomplete");
    refused("garbage.part", std::string(4096, 'x'), "not a WAV take");
    refused("empty.part", original.substr(0, data), "no complete audio frame");
    {
        SF_INFO info{}; info.samplerate = 48000; info.channels = 2; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        SNDFILE* file = sf_open((dir / "pcm.wav").c_str(), SFM_WRITE, &info);
        std::vector<short> zeros(2000); sf_writef_short(file, zeros.data(), 1000); sf_close(file);
    }
    refused("pcm.part", bytes(dir / "pcm.wav"), "not a 32-bit float take");
    // The .part a write failure leaves has a finished header; it recovers too.
    const auto failed = recoverTake(dir / "failed.wav.part", dir / "failed recovered.wav");
    require(failed.recovered && failed.frames >= 20 * 257 && failed.discarded_bytes == 0, "a finished .part file recovers");
}
}

int main() {
    const auto dir = std::filesystem::temp_directory_path() / ("fl-live-capture-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    try {
        ringContract();
        contentAndCallbackDiscipline(dir);
        overflowAccounting(dir);
        writeFailure(dir);
        interruptedRecovery(dir);
        std::filesystem::remove_all(dir);
        std::cout << "PASS: all-or-nothing ring; 400 blocks written bit for bit with no allocation or file call in the capture thread; "
                     "overflow drops whole blocks with written + dropped = captured; a failed write keeps a readable .part file; "
                     "a take killed mid-recording recovers bit for bit without changing the .part or replacing a file; truncated and invalid files handled\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(dir);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
