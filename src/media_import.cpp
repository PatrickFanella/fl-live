#include "fl_live/media_import.hpp"
#include <sndfile.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <system_error>

namespace fl_live {
namespace {
WavImport failure(ImportError error, std::string message) {
    WavImport result;
    result.error = error;
    result.message = std::move(message);
    return result;
}
struct Closer { void operator()(SNDFILE* file) const noexcept { sf_close(file); } };
}

WavImport importWav(const std::filesystem::path& path, const WavImportPolicy& policy, const std::atomic<bool>* cancel) {
    if (policy.frame_limit == 0 || policy.frame_limit > WavImportPolicy::default_frame_limit ||
        policy.session_rate < 8000 || policy.session_rate > 384000)
        return failure(ImportError::invalid_policy, "Import policy is outside the supported range");
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status))
        return failure(ImportError::cannot_open, "Choose an existing regular WAV file");
    SF_INFO info{};
    const std::unique_ptr<SNDFILE, Closer> file(sf_open(path.c_str(), SFM_READ, &info));
    if (!file) {
        // libsndfile reports unknown or malformed headers at open time.
        return failure(ImportError::not_wav, "File is not a readable WAV file. Convert it to WAV (PCM or float) and import again");
    }
    const auto container = info.format & SF_FORMAT_TYPEMASK;
    if (container != SF_FORMAT_WAV && container != SF_FORMAT_WAVEX)
        return failure(ImportError::not_wav, "Only WAV files are supported. Convert the file to WAV and import again");
    switch (info.format & SF_FORMAT_SUBMASK) {
    case SF_FORMAT_PCM_U8: case SF_FORMAT_PCM_16: case SF_FORMAT_PCM_24: case SF_FORMAT_PCM_32:
    case SF_FORMAT_FLOAT: case SF_FORMAT_DOUBLE: break;
    default:
        return failure(ImportError::unsupported_encoding,
            "WAV encoding is not supported. Use 8/16/24/32-bit PCM or 32/64-bit float");
    }
    WavImport result;
    result.source_rate = info.samplerate > 0 ? static_cast<std::uint32_t>(info.samplerate) : 0;
    result.channels = info.channels > 0 ? static_cast<std::uint32_t>(info.channels) : 0;
    if (info.channels < 1 || info.channels > static_cast<int>(Engine::max_output_channels))
        return failure(ImportError::unsupported_channels,
            "Only mono or stereo WAV files are supported. Mix the file down to one or two channels");
    if (result.source_rate != policy.session_rate)
        return failure(ImportError::sample_rate_mismatch, "WAV sample rate is " + std::to_string(result.source_rate) +
            " Hz but the session runs at " + std::to_string(policy.session_rate) +
            " Hz. Resample the file; fl-live does not resample or warp imported audio");
    const auto channels = static_cast<std::size_t>(info.channels);
    // Header frame counts can be wrong; reserve within the limit and read at
    // most one frame past it so an overlong file is detected without growing.
    const auto declared = info.frames > 0 ? static_cast<std::size_t>(info.frames) : 0;
    if (declared > policy.frame_limit)
        return failure(ImportError::too_long, "WAV file is longer than the " + std::to_string(policy.frame_limit) +
            "-frame import limit. Trim it and import again");
    std::vector<float> samples;
    samples.reserve(declared * channels);
    constexpr std::size_t chunk_frames = 16384;
    std::vector<float> chunk(chunk_frames * channels);
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            return failure(ImportError::cancelled, "Import cancelled; the session was not changed");
        const auto read = sf_readf_float(file.get(), chunk.data(), static_cast<sf_count_t>(chunk_frames));
        if (read < 0) return failure(ImportError::read_failed, "Reading the WAV file failed; the session was not changed");
        if (read == 0) break;
        const auto frames = static_cast<std::size_t>(read);
        if (samples.size() / channels + frames > policy.frame_limit)
            return failure(ImportError::too_long, "WAV file is longer than the " + std::to_string(policy.frame_limit) +
                "-frame import limit. Trim it and import again");
        const auto end = chunk.begin() + static_cast<std::ptrdiff_t>(frames * channels);
        if (!std::all_of(chunk.begin(), end, [](float x) { return std::isfinite(x); }))
            return failure(ImportError::non_finite, "WAV file contains NaN or infinite samples and was refused");
        samples.insert(samples.end(), chunk.begin(), end);
    }
    if (sf_error(file.get()) != SF_ERR_NO_ERROR)
        return failure(ImportError::read_failed, "Reading the WAV file failed; the session was not changed");
    if (samples.empty()) return failure(ImportError::empty, "WAV file contains no audio frames");
    result.frames = samples.size() / channels;
    result.clip = AudioClip{std::move(samples), true, static_cast<std::uint32_t>(channels)};
    return result;
}

WavImportWorker::WavImportWorker(WavImportPolicy policy) : policy_(policy), thread_([this] { run(); }) {}
WavImportWorker::~WavImportWorker() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        cancel_running_.store(true);
    }
    wake_.notify_all();
    thread_.join();
}
std::uint64_t WavImportWorker::submit(std::filesystem::path path) {
    std::uint64_t ticket = 0;
    {
        const std::lock_guard lock(mutex_);
        ticket = next_ticket_++;
        pending_.push_back({ticket, std::move(path)});
    }
    wake_.notify_one();
    return ticket;
}
std::optional<WavImportWorker::Result> WavImportWorker::poll() {
    const std::lock_guard lock(mutex_);
    if (done_.empty()) return std::nullopt;
    auto result = std::move(done_.front());
    done_.pop_front();
    return result;
}
void WavImportWorker::cancel(std::uint64_t ticket) {
    const std::lock_guard lock(mutex_);
    if (ticket == running_ticket_) { cancel_running_.store(true); return; }
    const auto found = std::find_if(pending_.begin(), pending_.end(), [ticket](const Request& r) { return r.ticket == ticket; });
    if (found == pending_.end()) return;
    pending_.erase(found);
    done_.push_back({ticket, failure(ImportError::cancelled, "Import cancelled; the session was not changed")});
}
void WavImportWorker::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (stopping_) return;
        auto request = std::move(pending_.front());
        pending_.pop_front();
        running_ticket_ = request.ticket;
        cancel_running_.store(false);
        lock.unlock();
        auto import = importWav(request.path, policy_, &cancel_running_);
        lock.lock();
        running_ticket_ = 0;
        done_.push_back({request.ticket, std::move(import)});
    }
}
} // namespace fl_live
