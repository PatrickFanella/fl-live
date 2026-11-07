#pragma once
#include "fl_live/capture.hpp"
#include "fl_live/input_monitor.hpp"
#include "fl_live/controller.hpp"
#include <memory>
#include <string>

namespace fl_live {
struct PipeWireConfig {
    std::string remote; // explicit remote; never silently choose a desktop server
    std::string target;
    std::string input_target; // the desktop's capture stream links here when set
    std::string midi_input_target; // and its MIDI input here
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
    // Transport anchor (docs/recording.md): in the cycle starting at graph
    // frame anchor_graph the transport rendered anchor_transport first.
    // Valid once the transport has run with a graph clock.
    bool anchor_valid = false;
    std::uint64_t anchor_graph = 0, anchor_transport = 0;
};
class PipeWireBackend {
public:
    PipeWireBackend(RenderController& controller, PipeWireConfig config);
    ~PipeWireBackend();
    PipeWireBackend(const PipeWireBackend&) = delete;
    PipeWireBackend& operator=(const PipeWireBackend&) = delete;
    [[nodiscard]] BackendStatus status() const noexcept;
    // Input monitoring: the output callback mixes `monitor` in after the
    // engine renders. Returns once the data thread has left the previous one.
    void setMonitor(InputMonitor* monitor) noexcept;
    // MIDI input monitoring: live notes applied before each block.
    void setLive(LiveMidi* live) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// An audio input stream (docs/recording.md). It connects to the explicit
// remote as node "fl-live-input" with one (MONO) or two (FL/FR) F32 ports
// and links only where told (target, autoconnect or an external link). Each
// buffer goes to the current sink on PipeWire's data thread; without a sink
// input is discarded. Frame counts are per channel.
struct CaptureStreamStatus {
    int state;
    int error;
    std::uint64_t callbacks;
    std::uint64_t frames;
    std::uint64_t bad_buffers;
};
class PipeWireCapture {
public:
    explicit PipeWireCapture(PipeWireConfig config);
    ~PipeWireCapture();
    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;
    // Control side. Returns once the data thread can no longer be inside the
    // previous sink, so that sink may be finished or destroyed afterwards.
    void setSink(CaptureSink* sink) noexcept;
    [[nodiscard]] CaptureStreamStatus status() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// MIDI input node "fl-live-midi-input" (docs/recording.md). Note messages go
// to the current MidiSink, stamped with the graph clock position of their
// cycle plus their offset in it. Raw MIDI and UMP controls are both accepted;
// UMP is converted to MIDI 1.0 bytes on the data thread without allocation.
// It links where told, like the audio input: config.input_target with
// autoconnect, or an external link.
struct MidiStreamStatus {
    int state;
    int error;
    std::uint64_t callbacks;
    std::uint64_t events;
    std::uint64_t bad_buffers;
};
class PipeWireMidiCapture {
public:
    explicit PipeWireMidiCapture(PipeWireConfig config);
    ~PipeWireMidiCapture();
    PipeWireMidiCapture(const PipeWireMidiCapture&) = delete;
    PipeWireMidiCapture& operator=(const PipeWireMidiCapture&) = delete;
    // As PipeWireCapture::setSink: returns once the data thread has left the previous sink.
    void setSink(MidiSink* sink) noexcept;
    [[nodiscard]] MidiStreamStatus status() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fl_live
