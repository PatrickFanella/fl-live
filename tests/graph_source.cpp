// Test fixture for pipewire_placement.py: emits events at exact graph-clock
// frames, an independent timeline for recording placement (#45).
//   graph-source REMOTE
// Nodes "fixture-clicks" (stereo F32: a click is one frame of 0.5 left,
// 0.25 right) and "fixture-notes" (MIDI). After {"ready":true}, stdin lines
// schedule events by graph frame:
//   click FRAME
//   note FRAME PITCH VELOCITY LENGTH
// Each event is emitted in the cycle containing its frame, at its offset.
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <spa/control/control.h>
#include <spa/node/io.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {
struct Event { std::uint64_t frame; std::uint8_t status, data1, data2; };
struct Stream {
    pw_stream* stream = nullptr;
    spa_hook hook{};
    spa_io_position* position = nullptr;
};
std::array<Event, 512> clicks{}, midi{};
std::atomic<std::size_t> click_count{0}, midi_count{0};
Stream audio, notes;

void io_changed(void* data, std::uint32_t id, void* area, std::uint32_t size) {
    if (id == SPA_IO_Position) static_cast<Stream*>(data)->position = size >= sizeof(spa_io_position) ? static_cast<spa_io_position*>(area) : nullptr;
}
void audio_process(void*) {
    pw_buffer* buffer = pw_stream_dequeue_buffer(audio.stream);
    if (!buffer) return;
    auto& d = buffer->buffer->datas[0];
    const std::uint32_t frames = audio.position ? static_cast<std::uint32_t>(audio.position->clock.duration) : 256;
    const std::uint32_t count = std::min<std::uint32_t>(frames, d.maxsize / 8);
    auto* out = static_cast<float*>(d.data);
    std::memset(out, 0, count * 8);
    if (audio.position) {
        const auto start = audio.position->clock.position;
        for (std::size_t i = 0, n = click_count.load(std::memory_order_acquire); i < n; ++i)
            if (clicks[i].frame >= start && clicks[i].frame < start + count) { out[2 * (clicks[i].frame - start)] = 0.5F; out[2 * (clicks[i].frame - start) + 1] = 0.25F; }
    }
    d.chunk->offset = 0; d.chunk->stride = 8; d.chunk->size = count * 8;
    buffer->size = count;
    pw_stream_queue_buffer(audio.stream, buffer);
}
void notes_process(void*) {
    pw_buffer* buffer = pw_stream_dequeue_buffer(notes.stream);
    if (!buffer) return;
    auto& d = buffer->buffer->datas[0];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(d.data, d.maxsize);
    spa_pod_frame frame;
    spa_pod_builder_push_sequence(&builder, &frame, 0);
    if (notes.position) {
        const auto start = notes.position->clock.position, length = notes.position->clock.duration;
        for (std::size_t i = 0, n = midi_count.load(std::memory_order_acquire); i < n; ++i) {
            const auto& e = midi[i];
            if (e.frame < start || e.frame >= start + length) continue;
            spa_pod_builder_control(&builder, static_cast<std::uint32_t>(e.frame - start), SPA_CONTROL_Midi);
            const std::uint8_t bytes[3]{e.status, e.data1, e.data2};
            spa_pod_builder_bytes(&builder, bytes, 3);
        }
    }
    spa_pod_builder_pop(&builder, &frame);
    d.chunk->offset = 0; d.chunk->stride = 1; d.chunk->size = builder.state.offset;
    pw_stream_queue_buffer(notes.stream, buffer);
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: graph-source REMOTE\n"; return 2; }
    pw_init(&argc, &argv);
    auto* loop = pw_thread_loop_new("graph-source", nullptr);
    auto* context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    pw_thread_loop_lock(loop);
    pw_thread_loop_start(loop);
    auto* core = pw_context_connect(context, pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], nullptr), 0);
    if (!core) { std::cerr << "cannot connect\n"; return 1; }
    static pw_stream_events audio_events{}, note_events{};
    audio_events.version = note_events.version = PW_VERSION_STREAM_EVENTS;
    audio_events.process = audio_process; audio_events.io_changed = io_changed;
    note_events.process = notes_process; note_events.io_changed = io_changed;
    audio.stream = pw_stream_new(core, "fixture-clicks", pw_properties_new(PW_KEY_NODE_NAME, "fixture-clicks", PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_NODE_GROUP, "pipewire.test", PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr));
    pw_stream_add_listener(audio.stream, &audio.hook, &audio_events, &audio);
    notes.stream = pw_stream_new(core, "fixture-notes", pw_properties_new(PW_KEY_NODE_NAME, "fixture-notes", PW_KEY_MEDIA_TYPE, "Midi",
        PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_FORMAT_DSP, "8 bit raw midi",
        PW_KEY_NODE_GROUP, "pipewire.test", PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr));
    pw_stream_add_listener(notes.stream, &notes.hook, &note_events, &notes);
    alignas(8) std::array<std::byte, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_F32; format.rate = 48000; format.channels = 2;
    format.position[0] = SPA_AUDIO_CHANNEL_FL; format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* audio_params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const spa_pod* note_params[] = {static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control)))};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(audio.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, audio_params, 1) < 0 ||
        pw_stream_connect(notes.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, note_params, 1) < 0) { std::cerr << "cannot connect streams\n"; return 1; }
    pw_thread_loop_unlock(loop);
    std::cout << "{\"ready\": true}" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string kind;
        std::uint64_t at = 0;
        in >> kind >> at;
        if (kind == "click" && click_count.load() < clicks.size()) {
            clicks[click_count.load()] = {at, 0, 0, 0};
            click_count.fetch_add(1, std::memory_order_release);
        } else if (kind == "note" && midi_count.load() + 2 <= midi.size()) {
            unsigned pitch = 0, velocity = 0; std::uint64_t length = 0;
            in >> pitch >> velocity >> length;
            midi[midi_count.load()] = {at, 0x90, static_cast<std::uint8_t>(pitch), static_cast<std::uint8_t>(velocity)};
            midi[midi_count.load() + 1] = {at + length, 0x80, static_cast<std::uint8_t>(pitch), 0};
            midi_count.fetch_add(2, std::memory_order_release);
        } else if (kind == "quit") break;
        std::cout << "{\"ok\": true}" << std::endl;
    }
    pw_thread_loop_stop(loop);
    return 0;
}
