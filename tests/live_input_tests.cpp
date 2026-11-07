// Live input notes for MIDI monitoring: voices sound without a clip or a
// running transport, release on note-off, survive transport stop, end on
// silence_live(), follow their track through replace(), and neither the
// engine nor LiveMidi allocates while rendering.
#include "fl_live/controller.hpp"
#include "fl_live/input_monitor.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
thread_local bool watching = false;
std::atomic<std::size_t> allocations{0};
}
void* operator new(std::size_t size) {
    if (watching) allocations.fetch_add(1);
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace fl_live;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
SessionPlan plan(float pan0, float pan1) {
    SessionPlan result{48000, 120000, {}};
    MidiClip clip{4800, {}, true, Instrument{Waveform::square, 0, 0, 1.0F, 100}};
    Track first{1.0F, {clip}, pan0};
    first.live_instrument = clip.instrument;
    Track second{0.5F, {clip}, pan1};
    second.live_instrument = Instrument{Waveform::square, 0, 0, 1.0F, 0};
    result.tracks = {first, second};
    return result;
}
void engineLiveVoices() {
    const auto a = plan(-1.0F, 1.0F);
    Engine engine(a);
    std::vector<float> out(2 * 480);
    watching = true;
    engine.live(0, 69, 1.0F, true);           // transport stopped, no clip
    (void)engine.process(out, 2);
    watching = false;
    require(engine.active_voices() == 1, "a live note sounds without a clip or transport");
    // Square wave at velocity 1 (level 0.1), track gain 1, pan hard left.
    bool square = true;
    for (std::size_t f = 0; f < 480; ++f) square = square && std::abs(std::abs(out[2 * f]) - 0.1F) < 1e-6F && out[2 * f + 1] == 0;
    require(square, "the live voice plays the track's live instrument at its gain and pan");
    watching = true;
    engine.live(0, 69, 0, false);              // note-off: 100-frame release
    (void)engine.process(std::span(out).first(2 * 99), 2);
    watching = false;
    require(engine.active_voices() == 1, "releasing until the release ends");
    (void)engine.process(std::span(out).first(2), 2);
    require(engine.active_voices() == 0, "the release ends");
    // Transport stop keeps live voices; silence_live ends them.
    engine.live(0, 60, 0.5F, true);
    engine.live(1, 64, 0.5F, true);
    engine.start(); (void)engine.process(out, 2); engine.stop(); (void)engine.process(out, 2);
    require(engine.active_voices() == 2, "transport stop keeps playing keys");
    // Replacement swapping the two tracks: voices follow their track.
    const auto b = plan(1.0F, -1.0F);
    PlanMapping swap{{1, 0}, {{0}, {0}}};
    watching = true;
    (void)engine.replace(b, swap);
    (void)engine.process(out, 2);
    watching = false;
    require(engine.active_voices() == 2, "live voices follow their tracks through replace");
    // Track 1 of b holds old track 0's voice (gain 0.5, pan -1: left only);
    // track 0 of b holds old track 1's (gain 1, pan +1: right only).
    require(std::abs(out[0]) > 0 && std::abs(out[1]) > 0, "both moved voices sound");
    engine.silence_live();
    (void)engine.process(out, 2);
    require(engine.active_voices() == 0, "silence_live ends every live voice");
    engine.live(7, 60, 1.0F, true);
    engine.live(0, 200, 1.0F, true);
    require(engine.active_voices() == 0, "an unknown track or pitch is ignored");
    // Eight voices per track; a ninth takes the oldest.
    for (std::uint8_t p = 40; p < 49; ++p) { engine.live(0, p, 1.0F, true); (void)engine.process(std::span(out).first(2), 2); }
    require(engine.active_voices() == Engine::live_voices_per_track, "eight live voices per track");
    require(allocations.load() == 0, "no allocation while rendering live voices");
}
// LiveMidi through RenderController: note messages reach the masked tracks
// at the next block; a mask change silences first; panic silences too.
void liveMidiPath() {
    const auto a = plan(0, 0);
    RenderController controller(a);
    LiveMidi live;
    std::array<float, 2 * 256> out{};
    const auto send = [&](std::uint8_t s, std::uint8_t d1, std::uint8_t d2) { const std::array<std::uint8_t, 3> m{s, d1, d2}; live.midi(0, m); };
    live.setTracks(0b10);
    watching = true;
    send(0x90, 60, 127);
    send(0xB0, 1, 2);    // ignored
    live.apply(controller); controller.process(out, 2);
    watching = false;
    require(controller.status().active_voices == 1, "a note reaches the masked track");
    send(0x80, 60, 0);
    watching = true;
    live.apply(controller); controller.process(out, 2);
    watching = false;
    require(controller.status().active_voices == 0, "note-off (track 1 has no release)");
    send(0x90, 62, 100);
    live.apply(controller); controller.process(out, 2);
    live.setTracks(0b01);  // monitoring moves: the held note is silenced
    live.apply(controller); controller.process(out, 2);
    require(controller.status().active_voices == 0, "a mask change silences live voices");
    send(0x90, 62, 100);
    live.apply(controller); controller.process(out, 2);
    require(controller.status().active_voices == 1, "the new track plays");
    controller.panic(); controller.process(out, 2);
    require(controller.status().active_voices == 0, "panic silences live voices");
    for (int i = 0; i < 300; ++i) send(0x90, 50, 100);
    require(live.dropped() > 0, "a full ring refuses and counts notes");
    require(allocations.load() == 0, "no allocation on the live path");
}
}

int main() {
    try {
        engineLiveVoices();
        liveMidiPath();
        std::cout << "PASS: live notes sound without a clip or transport at the track's live instrument, gain and pan, release on note-off, "
                     "survive transport stop, follow their track through replace, end on silence_live and panic; LiveMidi applies them "
                     "to the masked tracks and silences on a mask change; no allocation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
