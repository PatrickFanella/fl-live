#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace fl_live {
// Single-producer/single-consumer ring of interleaved float samples with a
// fixed, preallocated capacity. push() is all or nothing: a block that does
// not fit is refused whole, never split. Neither side allocates or locks.
class CaptureRing {
public:
    explicit CaptureRing(std::size_t samples);
    [[nodiscard]] bool push(std::span<const float> block) noexcept;
    [[nodiscard]] std::size_t pop(std::span<float> out) noexcept; // up to out.size() samples
    [[nodiscard]] std::size_t capacity() const noexcept { return data_.size() - 1; }
private:
    std::vector<float> data_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

struct CaptureStatus {
    std::uint64_t frames_written = 0; // frames on disk
    std::uint64_t frames_dropped = 0; // frames refused by the full ring
    std::uint64_t blocks_dropped = 0;
    bool overflowed = false;
    bool failed = false;   // a write failed; later frames are discarded
    bool finished = false;
    std::string error;     // actionable text when failed
    std::filesystem::path file; // the finished file, or the .part file it was left as
};

// One recorded take (docs/recording.md). The audio callback calls capture();
// a worker thread writes 32-bit float WAV to "<path>.part" and finish()
// renames it to <path>.
//
// - Overflow: a block that does not fit in the ring is dropped whole and
//   counted; the take is marked overflowed. The file holds every block that
//   fit, in order; dropped blocks leave no gap marker.
// - Write failure: writing stops, frames already written stay in the .part
//   file, which finish() closes but does not rename, and the error is
//   reported. Capture keeps returning without blocking.
// - The take never replaces a file: an existing <path> or <path>.part is
//   refused at construction, and finish() keeps the .part name if <path>
//   appeared meanwhile.
class CaptureTake {
public:
    CaptureTake(std::filesystem::path path, std::uint32_t channels, std::uint32_t rate, std::size_t ring_frames = 48000 * 2);
    CaptureTake(const CaptureTake&) = delete;
    CaptureTake& operator=(const CaptureTake&) = delete;
    ~CaptureTake();
    // Callback side: no allocation, lock, file I/O or wait.
    void capture(std::span<const float> interleaved) noexcept;
    // Control side: stops accepting, writes what the ring holds, closes and
    // renames. Repeated calls return the same status.
    CaptureStatus finish();
    [[nodiscard]] CaptureStatus status() const;
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
private:
    void run() noexcept;
    std::filesystem::path path_, part_;
    std::uint32_t channels_, rate_;
    CaptureRing ring_;
    std::atomic<bool> accepting_{true}, stop_{false};
    std::atomic<std::uint64_t> written_{0}, dropped_frames_{0}, dropped_blocks_{0};
    std::atomic<bool> failed_{false};
    mutable std::mutex mutex_; // worker/control only: error text and final status
    std::string error_;
    void* file_ = nullptr; // SNDFILE*, worker-owned until finish()
    CaptureStatus final_{};
    bool done_ = false;
    std::thread worker_;
};
} // namespace fl_live
