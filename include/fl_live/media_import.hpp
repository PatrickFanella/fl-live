#pragma once
#include "fl_live/engine.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fl_live {
// Declared WAV admission policy. Decoding happens on a control or worker
// thread, never in the render callback; the result is an independent
// AudioClip copy that preparation then owns.
//
// - Container: RIFF/WAVE or WAVE_FORMAT_EXTENSIBLE only.
// - Encoding: unsigned 8-bit, signed 16/24/32-bit PCM, 32/64-bit float.
//   Integer PCM is normalized to [-1, 1); float values are kept as stored and
//   must be finite (they may exceed 1; the engine mix is unclipped).
// - Channels: one (mono) or two (interleaved left/right); more are refused.
// - Sample rate: must equal the session rate. There is no resampling and no
//   warping, so a mismatched file is refused rather than played at the wrong
//   speed.
// - Length: 1 to frame_limit frames; frame_limit * channels samples are kept.
struct WavImportPolicy {
    static constexpr std::size_t default_frame_limit = 1U << 24;
    std::uint32_t session_rate = 48000;
    std::size_t frame_limit = default_frame_limit;
};
enum class ImportError {
    none, cannot_open, not_wav, unsupported_encoding, unsupported_channels,
    sample_rate_mismatch, empty, too_long, read_failed, non_finite, cancelled, invalid_policy
};
struct WavImport {
    ImportError error = ImportError::none;
    std::string message; // actionable text for the UI; empty on success
    std::uint32_t source_rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    AudioClip clip;      // valid only when error == none
    explicit operator bool() const noexcept { return error == ImportError::none; }
};
// Blocking decode. Allocates, opens and reads the file; call off the callback.
// A set cancel flag is checked between bounded read chunks.
WavImport importWav(const std::filesystem::path& path, const WavImportPolicy& policy = {},
    const std::atomic<bool>* cancel = nullptr);

// One worker thread decodes queued requests in order. submit/poll/cancel are
// called by the single control owner; poll never blocks on decoding. The
// destructor cancels the current request and joins the thread. Results do not
// touch any document: the caller applies a successful clip to a candidate and
// keeps its current state on failure.
class WavImportWorker {
public:
    struct Result { std::uint64_t ticket; WavImport import; };
    explicit WavImportWorker(WavImportPolicy policy = {});
    ~WavImportWorker();
    WavImportWorker(const WavImportWorker&) = delete;
    WavImportWorker& operator=(const WavImportWorker&) = delete;
    std::uint64_t submit(std::filesystem::path path);
    std::optional<Result> poll();
    void cancel(std::uint64_t ticket); // pending: dropped as cancelled; running: interrupted
    // Thread that performs decoding, for ownership checks in tests.
    std::thread::id worker_id() const noexcept { return thread_.get_id(); }
private:
    struct Request { std::uint64_t ticket; std::filesystem::path path; };
    const WavImportPolicy policy_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Request> pending_;
    std::deque<Result> done_;
    std::uint64_t next_ticket_ = 1, running_ticket_ = 0;
    std::atomic<bool> cancel_running_{false};
    bool stopping_ = false;
    std::thread thread_;
    void run();
};
} // namespace fl_live
