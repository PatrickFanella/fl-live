#pragma once
#include "fl_live/controller.hpp"
#include <memory>
#include <string>

namespace fl_live {
struct PipeWireConfig {
    std::string remote; // explicit remote; never silently choose a desktop server
    std::string target;
    std::string node_group;
    bool autoconnect = false;
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 1; // 1 = MONO downmix, 2 = interleaved FL/FR; others are refused
};
// Frame counts are per channel: one stereo frame is two samples.
struct BackendStatus {
    int state;
    int error;
    std::uint64_t callbacks;
    std::uint64_t frames;
    std::uint64_t bad_buffers;
    std::uint64_t max_process_ns;
    std::uint64_t processing_over_budget;
    std::uint64_t min_render_frames;
    std::uint64_t max_render_frames;
};
class PipeWireBackend {
public:
    PipeWireBackend(RenderController& controller, PipeWireConfig config);
    ~PipeWireBackend();
    PipeWireBackend(const PipeWireBackend&) = delete;
    PipeWireBackend& operator=(const PipeWireBackend&) = delete;
    [[nodiscard]] BackendStatus status() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fl_live
