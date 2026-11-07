#include "fl_live/capture.hpp"
#include <sndfile.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace fl_live {
CaptureRing::CaptureRing(std::size_t samples) : data_(samples + 1) {
    if (samples == 0) throw std::invalid_argument("capture ring needs capacity");
}
bool CaptureRing::push(std::span<const float> block) noexcept {
    const auto size = data_.size();
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    const auto used = (head + size - tail) % size;
    if (block.size() > size - 1 - used) return false;
    const auto first = std::min(block.size(), size - head);
    std::copy_n(block.begin(), first, data_.begin() + static_cast<std::ptrdiff_t>(head));
    std::copy(block.begin() + static_cast<std::ptrdiff_t>(first), block.end(), data_.begin());
    head_.store((head + block.size()) % size, std::memory_order_release);
    return true;
}
std::size_t CaptureRing::pop(std::span<float> out) noexcept {
    const auto size = data_.size();
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto head = head_.load(std::memory_order_acquire);
    const auto available = std::min((head + size - tail) % size, out.size());
    const auto first = std::min(available, size - tail);
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(tail), first, out.begin());
    std::copy_n(data_.begin(), available - first, out.begin() + static_cast<std::ptrdiff_t>(first));
    tail_.store((tail + available) % size, std::memory_order_release);
    return available;
}

CaptureTake::CaptureTake(std::filesystem::path path, std::uint32_t channels, std::uint32_t rate, std::size_t ring_frames)
    : path_(std::move(path)), part_(path_.string() + ".part"), channels_(channels), rate_(rate),
      ring_((channels == 1 || channels == 2) && ring_frames ? ring_frames * channels : 1) {
    if (channels != 1 && channels != 2) throw std::invalid_argument("A take records one or two channels");
    if (rate < 8000 || rate > 384000 || ring_frames == 0) throw std::invalid_argument("Invalid capture rate or buffer");
    std::error_code ignored;
    if (std::filesystem::exists(path_, ignored) || std::filesystem::exists(part_, ignored))
        throw std::invalid_argument("A recording never replaces a file: " + path_.filename().string() + " or its .part file already exists");
    SF_INFO info{};
    info.samplerate = static_cast<int>(rate);
    info.channels = static_cast<int>(channels);
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    file_ = sf_open(part_.c_str(), SFM_WRITE, &info);
    if (!file_) throw std::runtime_error("Cannot create " + part_.filename().string() + ": " + sf_strerror(nullptr));
    worker_ = std::thread([this] { run(); });
}
CaptureTake::~CaptureTake() { (void)finish(); }

void CaptureTake::cycle(std::uint64_t frame, std::uint32_t) noexcept {
    if (!accepting_.load(std::memory_order_acquire)) return;
    auto expected = no_origin;
    origin_.compare_exchange_strong(expected, frame, std::memory_order_release, std::memory_order_relaxed);
}
void CaptureTake::capture(std::span<const float> interleaved) noexcept {
    if (!accepting_.load(std::memory_order_acquire) || interleaved.empty()) return;
    if (!ring_.push(interleaved)) {
        dropped_frames_.fetch_add(interleaved.size() / channels_, std::memory_order_relaxed);
        dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
    }
}

void CaptureTake::run() noexcept {
    std::array<float, 8192> chunk{};
    const auto usable = chunk.size() / channels_ * channels_; // whole frames only
    for (;;) {
        const bool stopping = stop_.load(std::memory_order_acquire);
        const auto got = ring_.pop(std::span(chunk).first(usable));
        if (got && !failed_.load(std::memory_order_relaxed)) {
            const auto frames = static_cast<sf_count_t>(got / channels_);
            const auto wrote = sf_writef_float(static_cast<SNDFILE*>(file_), chunk.data(), frames);
            if (wrote > 0) written_.fetch_add(static_cast<std::uint64_t>(wrote), std::memory_order_relaxed);
            if (wrote != frames) {
                std::lock_guard lock(mutex_);
                error_ = std::string("Writing the recording failed: ") + sf_strerror(static_cast<SNDFILE*>(file_)) +
                    ". The frames recorded so far are kept in " + part_.filename().string() + ".";
                failed_.store(true, std::memory_order_release);
            }
        }
        if (!got) {
            if (stopping) return; // drained after stop
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

CaptureStatus CaptureTake::status() const {
    std::lock_guard lock(mutex_);
    if (done_) return final_;
    CaptureStatus value;
    value.frames_written = written_.load();
    value.frames_dropped = dropped_frames_.load();
    value.blocks_dropped = dropped_blocks_.load();
    value.overflowed = value.blocks_dropped > 0;
    value.failed = failed_.load();
    value.error = error_;
    value.file = part_;
    return value;
}

CaptureStatus CaptureTake::finish() {
    {
        std::lock_guard lock(mutex_);
        if (done_) return final_;
    }
    accepting_.store(false, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    // The header is completed on close even after a failed write, so the
    // frames written so far stay readable.
    const int closed = sf_close(static_cast<SNDFILE*>(file_));
    file_ = nullptr;
    auto result = status();
    if (closed != 0 && !result.failed) {
        result.failed = true;
        result.error = "Closing the recording failed. The frames recorded so far are kept in " + part_.filename().string() + ".";
    }
    std::error_code error;
    if (!result.failed && !std::filesystem::exists(path_, error)) {
        std::filesystem::rename(part_, path_, error);
        if (!error) result.file = path_;
    }
    result.finished = true;
    std::lock_guard lock(mutex_);
    final_ = result;
    done_ = true;
    return final_;
}

MidiTake::MidiTake(std::size_t capacity) : ring_(capacity + 1) {
    if (!capacity) throw std::invalid_argument("MIDI take needs capacity");
    events_.reserve(capacity);
}
void MidiTake::cycle(std::uint64_t frame, std::uint32_t frames) noexcept {
    if (!accepting_.load(std::memory_order_acquire)) return;
    auto expected = no_origin;
    origin_.compare_exchange_strong(expected, frame, std::memory_order_release, std::memory_order_relaxed);
    end_.store(frame + frames, std::memory_order_release);
}
void MidiTake::midi(std::uint64_t frame, std::span<const std::uint8_t> message) noexcept {
    if (!accepting_.load(std::memory_order_acquire) || message.size() < 3) return;
    const auto kind = message[0] & 0xF0;
    if (kind != 0x80 && kind != 0x90) return;
    const auto size = ring_.size();
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = (head + 1) % size;
    if (next == tail_.load(std::memory_order_acquire)) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
    ring_[head] = {frame, message[0], static_cast<std::uint8_t>(message[1] & 0x7F), static_cast<std::uint8_t>(message[2] & 0x7F)};
    head_.store(next, std::memory_order_release);
}
void MidiTake::drain() {
    const auto size = ring_.size();
    auto tail = tail_.load(std::memory_order_relaxed);
    const auto head = head_.load(std::memory_order_acquire);
    while (tail != head) { events_.push_back(ring_[tail]); tail = (tail + 1) % size; }
    tail_.store(tail, std::memory_order_release);
}
MidiTakeStatus MidiTake::finish() {
    if (done_) return final_;
    accepting_.store(false, std::memory_order_release);
    drain();
    done_ = true;
    const auto origin = origin_.load();
    if (origin == ~std::uint64_t{0}) { final_.dropped = dropped_.load(); return final_; } // never ran a cycle
    const auto end = std::max(end_.load(), origin);
    final_.frames = end - origin;
    final_.dropped = dropped_.load();
    final_.events = events_.size();
    std::array<std::optional<RecordedNote>, 128> held{};
    const auto close = [&](std::uint8_t pitch, std::uint64_t at) {
        auto& note = held[pitch];
        if (!note) return;
        note->length = std::max<std::uint64_t>(at > note->start ? at - note->start : 0, 1);
        final_.notes.push_back(*note);
        note.reset();
    };
    // Events keep arrival order; frames from one clock never go back, but
    // clamp into the take so a late cycle stamp cannot make a negative time.
    for (const auto& e : events_) {
        const auto at = std::min(std::max(e.frame, origin), end) - origin;
        const bool on = (e.status & 0xF0) == 0x90 && e.data2 > 0;
        close(e.data1, at);
        if (on) held[e.data1] = RecordedNote{at, 0, e.data1, e.data2};
    }
    for (std::uint8_t pitch = 0; pitch < 128; ++pitch)
        if (held[pitch]) { close(pitch, final_.frames); ++final_.closed_at_end; }
    std::stable_sort(final_.notes.begin(), final_.notes.end(),
        [](const RecordedNote& a, const RecordedNote& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
    return final_;
}

namespace {
std::uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
}
TakeRecovery recoverTake(const std::filesystem::path& part, const std::filesystem::path& destination) {
    TakeRecovery result;
    const auto name = part.filename().string();
    const auto refuse = [&](const std::string& why) { result.message = "Cannot recover " + name + ": " + why; return result; };
    std::error_code error;
    const auto size = std::filesystem::file_size(part, error);
    if (error) return refuse("the file cannot be read (" + error.message() + ")");
    std::ifstream in(part, std::ios::binary);
    if (!in) return refuse("the file cannot be opened");
    unsigned char head[12];
    if (!in.read(reinterpret_cast<char*>(head), 12) || std::memcmp(head, "RIFF", 4) || std::memcmp(head + 8, "WAVE", 4))
        return refuse("it is not a WAV take");
    std::uint64_t at = 12, data = 0, declared = 0;
    bool format = false;
    while (!data) {
        unsigned char chunk[8];
        if (at + 8 > size || !in.read(reinterpret_cast<char*>(chunk), 8)) return refuse("the header is incomplete");
        const auto length = le32(chunk + 4);
        at += 8;
        if (!std::memcmp(chunk, "data", 4)) { data = at; declared = length; break; }
        if (at + length > size) return refuse("the header is incomplete");
        if (!std::memcmp(chunk, "fmt ", 4)) {
            unsigned char fmt[40]{};
            if (length < 16 || !in.read(reinterpret_cast<char*>(fmt), std::min<std::uint32_t>(length, 40))) return refuse("the format block is invalid");
            auto tag = le16(fmt);
            if (tag == 0xFFFE && length >= 26) tag = le16(fmt + 24); // WAVE_FORMAT_EXTENSIBLE subformat
            result.channels = le16(fmt + 2);
            result.rate = le32(fmt + 4);
            if (tag != 3 || le16(fmt + 14) != 32) return refuse("it is not a 32-bit float take");
            if (result.channels != 1 && result.channels != 2) return refuse("takes have one or two channels");
            if (result.rate < 8000 || result.rate > 384000) return refuse("the sample rate is invalid");
            format = true;
        }
        at += length + (length & 1);
        in.seekg(static_cast<std::streamoff>(at));
    }
    if (!format) return refuse("the format block is missing");
    // A finished header states the data size; an interrupted one does not.
    const auto available = size - data;
    const auto bytes = declared && declared <= available ? std::uint64_t{declared} : available;
    const auto frame = 4ULL * result.channels;
    result.frames = bytes / frame;
    result.discarded_bytes = bytes % frame;
    if (!result.frames) return refuse("it holds no complete audio frame");
    if (std::filesystem::exists(destination, error)) return refuse(destination.filename().string() + " already exists and is never replaced");
    const auto temporary = std::filesystem::path(destination.string() + ".recovering");
    if (std::filesystem::exists(temporary, error)) return refuse(temporary.filename().string() + " already exists");
    SF_INFO info{};
    info.samplerate = static_cast<int>(result.rate);
    info.channels = static_cast<int>(result.channels);
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    auto* out = sf_open(temporary.c_str(), SFM_WRITE, &info);
    if (!out) return refuse(std::string("cannot create ") + temporary.filename().string() + ": " + sf_strerror(nullptr));
    in.clear();
    in.seekg(static_cast<std::streamoff>(data));
    std::vector<float> chunk(8192 * result.channels);
    std::uint64_t left = result.frames;
    bool ok = true;
    while (left && ok) {
        const auto frames = std::min<std::uint64_t>(left, 8192);
        ok = static_cast<bool>(in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(frames * frame))) &&
             sf_writef_float(out, chunk.data(), static_cast<sf_count_t>(frames)) == static_cast<sf_count_t>(frames);
        left -= frames;
    }
    ok = sf_close(out) == 0 && ok;
    // Read back before publishing, then link without replacing.
    if (ok) {
        SF_INFO check{};
        auto* back = sf_open(temporary.c_str(), SFM_READ, &check);
        ok = back && check.frames == static_cast<sf_count_t>(result.frames) && check.channels == info.channels;
        if (back) sf_close(back);
    }
    if (ok) {
        std::filesystem::create_hard_link(temporary, destination, error);
        if (error) { std::filesystem::remove(temporary, error); return refuse(destination.filename().string() + " could not be created and nothing was replaced"); }
    }
    std::filesystem::remove(temporary, error);
    if (!ok) return refuse("writing the recovered file failed; the .part file is unchanged");
    result.recovered = true;
    result.file = destination;
    return result;
}
} // namespace fl_live
