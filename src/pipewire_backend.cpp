#include "fl_live/pipewire_backend.hpp"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace fl_live {
struct PipeWireBackend::Impl {
    RenderController& controller;
    PipeWireConfig config;
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook core_hook{}, stream_hook{};
    std::atomic<int> state{PW_STREAM_STATE_UNCONNECTED}, error{0};
    std::atomic<bool> format_ok{false};
    std::atomic<std::uint64_t> callbacks{0}, frames{0}, bad_buffers{0}, max_ns{0}, over_budget{0};
    std::atomic<std::uint64_t> min_render_frames{0}, max_render_frames{0};
    bool started = false, initialized = false;

    Impl(RenderController& value, PipeWireConfig settings) : controller(value), config(std::move(settings)) {}
    ~Impl() {
        // Joining the loop/context processing before transferring ownership back
        // to control is mandatory. PipeWire destroys data-loop callbacks here.
        if (loop && started) pw_thread_loop_stop(loop);
        if (stream) { spa_hook_remove(&stream_hook); pw_stream_destroy(stream); }
        if (core) { spa_hook_remove(&core_hook); pw_core_disconnect(core); }
        if (context) pw_context_destroy(context);
        if (loop) pw_thread_loop_destroy(loop);
        if (initialized) pw_deinit();
        controller.finish();
    }
    static void state_changed(void* data, pw_stream_state, pw_stream_state next, const char*) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.state.store(next);
        if (next == PW_STREAM_STATE_ERROR) { self.error.store(-EIO); self.controller.panic(); }
    }
    static void core_error(void* data, std::uint32_t, int, int result, const char*) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (result < 0) { self.error.store(result); self.controller.panic(); }
    }
    static void param_changed(void* data, std::uint32_t id, const spa_pod* parameter) noexcept {
        if (id != SPA_PARAM_Format || !parameter) return;
        auto& self = *static_cast<Impl*>(data);
        spa_audio_info_raw format{};
        const bool valid = spa_format_audio_raw_parse(parameter, &format) >= 0 &&
            format.format == SPA_AUDIO_FORMAT_F32 && format.channels == self.config.channels &&
            format.rate == self.config.sample_rate;
        self.format_ok.store(valid);
        if (!valid) { self.error.store(-ENOTSUP); self.controller.panic(); }
    }
    static void process(void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        const auto start = std::chrono::steady_clock::now();
        pw_buffer* buffer = pw_stream_dequeue_buffer(self.stream);
        if (!buffer) { self.bad_buffers.fetch_add(1); return; }
        auto* raw = buffer->buffer;
        if (!raw || raw->n_datas != 1 || !raw->datas[0].data || !raw->datas[0].chunk) {
            self.bad_buffers.fetch_add(1);
            pw_stream_queue_buffer(self.stream, buffer);
            return;
        }
        auto& audio = raw->datas[0];
        const std::size_t channels = self.config.channels;
        const auto stride = channels * sizeof(float);
        const auto capacity = audio.maxsize / stride;
        const auto count = static_cast<std::size_t>(buffer->requested ? std::min<std::uint64_t>(buffer->requested, capacity) : capacity);
        audio.chunk->offset = 0;
        audio.chunk->stride = static_cast<std::int32_t>(stride);
        audio.chunk->size = static_cast<std::uint32_t>(count * stride);
        auto output = std::span(static_cast<float*>(audio.data), count * channels);
        if (count > 8192) {
            // Do not turn a protocol/quantum failure into an unbounded clear.
            audio.chunk->size = 0;
            self.bad_buffers.fetch_add(1);
            self.error.store(-EMSGSIZE);
            self.controller.panic();
        } else if (self.error.load() || !self.format_ok.load()) {
            // Errors remain latched until teardown/reconnect. A later valid
            // buffer must not resume audio while control observes the failure.
            std::fill(output.begin(), output.end(), 0);
            self.bad_buffers.fetch_add(1);
        } else {
            if (count && (!self.min_render_frames.load() || count < self.min_render_frames.load()))
                self.min_render_frames.store(count);
            if (count > self.max_render_frames.load()) self.max_render_frames.store(count);
            self.controller.process(output, channels);
        }
        const auto submitted = audio.chunk->size / stride;
        self.frames.fetch_add(submitted);
        self.callbacks.fetch_add(1);
        buffer->size = submitted;
        const int queued = pw_stream_queue_buffer(self.stream, buffer);
        if (queued < 0) { self.error.store(queued); self.controller.panic(); }
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        if (elapsed > self.max_ns.load()) self.max_ns.store(elapsed);
        if (elapsed > count * 1000000000ULL / self.config.sample_rate) self.over_budget.fetch_add(1);
    }
    void connect() {
        if (config.remote.empty()) throw std::invalid_argument("PipeWire remote must be explicit");
        if (config.sample_rate != controller.sample_rate()) throw std::invalid_argument("backend rate must match prepared session");
        if (config.channels < 1 || config.channels > Engine::max_output_channels)
            throw std::invalid_argument("backend supports one (MONO) or two (FL/FR) output channels");
        pw_init(nullptr, nullptr);
        initialized = true;
        loop = pw_thread_loop_new("fl-live-control", nullptr);
        if (!loop) throw std::runtime_error("cannot create PipeWire loop");
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        if (!context) throw std::runtime_error("cannot create PipeWire context");
        core = pw_context_connect(context, pw_properties_new(PW_KEY_REMOTE_NAME, config.remote.c_str(), nullptr), 0);
        if (!core) throw std::runtime_error("cannot connect to requested PipeWire remote");
        static const pw_core_events core_events = [] {
            pw_core_events value{}; value.version = PW_VERSION_CORE_EVENTS; value.error = core_error; return value;
        }();
        pw_core_add_listener(core, &core_hook, &core_events, this);
        auto* properties = pw_properties_new(PW_KEY_NODE_NAME, "fl-live-output", PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr);
        if (!properties) throw std::runtime_error("cannot allocate PipeWire properties");
        if (!config.node_group.empty()) pw_properties_set(properties, PW_KEY_NODE_GROUP, config.node_group.c_str());
        if (!config.target.empty()) pw_properties_set(properties, PW_KEY_TARGET_OBJECT, config.target.c_str());
        stream = pw_stream_new(core, "fl-live-output", properties);
        if (!stream) throw std::runtime_error("cannot create PipeWire stream");
        static const pw_stream_events stream_events = [] {
            pw_stream_events value{}; value.version = PW_VERSION_STREAM_EVENTS;
            value.state_changed = state_changed; value.param_changed = param_changed; value.process = process; return value;
        }();
        pw_stream_add_listener(stream, &stream_hook, &stream_events, this);
        alignas(8) std::array<std::byte, 1024> storage{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
        spa_audio_info_raw format{};
        format.format = SPA_AUDIO_FORMAT_F32; format.rate = config.sample_rate; format.channels = config.channels;
        if (config.channels == 2) { format.position[0] = SPA_AUDIO_CHANNEL_FL; format.position[1] = SPA_AUDIO_CHANNEL_FR; }
        else format.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
        auto flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
        if (config.autoconnect) flags = flags | PW_STREAM_FLAG_AUTOCONNECT;
        const int result = pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(flags), params, 1);
        if (result < 0 || pw_thread_loop_start(loop) < 0) throw std::runtime_error("cannot start PipeWire stream");
        started = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (state.load() < PW_STREAM_STATE_PAUSED && error.load() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (state.load() < PW_STREAM_STATE_PAUSED || error.load()) throw std::runtime_error("PipeWire stream failed to become ready");
    }
};
PipeWireBackend::PipeWireBackend(RenderController& controller, PipeWireConfig config)
    : impl_(std::make_unique<Impl>(controller, std::move(config))) { impl_->connect(); }
PipeWireBackend::~PipeWireBackend() = default;
BackendStatus PipeWireBackend::status() const noexcept {
    return {impl_->state.load(), impl_->error.load(), impl_->callbacks.load(), impl_->frames.load(),
        impl_->bad_buffers.load(), impl_->max_ns.load(), impl_->over_budget.load(),
        impl_->min_render_frames.load(), impl_->max_render_frames.load()};
}
} // namespace fl_live
