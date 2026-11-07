#include "fl_live/capture.hpp"
#include <sndfile.h>
#include <algorithm>
#include <array>
#include <chrono>
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
} // namespace fl_live
