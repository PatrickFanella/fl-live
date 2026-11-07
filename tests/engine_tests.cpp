#include "fl_live/engine.hpp"
#include "fl_live/controller.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <tuple>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
bool watch_allocations = false;
std::size_t render_allocations = 0;
void counted() { if (watch_allocations) ++render_allocations; }
}
void* operator new(std::size_t size) {
    counted();
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    counted();
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(alignment), size ? size : 1) != 0) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size, std::align_val_t a) { return ::operator new(size, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {
using namespace fl_live;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void invalid(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid plan was accepted");
}
SessionPlan audio_plan() {
    SessionPlan plan;
    plan.tracks.push_back(Track{0.5F, {AudioClip{{1, 2, 3}, true}, AudioClip{{0}, true}}});
    return plan;
}
std::vector<float> render(const SessionPlan& plan, std::span<const Command> commands,
                          std::size_t block, std::size_t total) {
    Engine engine(plan);
    for (auto c : commands) require(engine.schedule(c) == ScheduleResult::accepted, "schedule failed");
    std::vector<float> output(total);
    engine.start();
    for (std::size_t at = 0; at < total; at += block) {
        const auto before = render_allocations;
        watch_allocations = true;
        engine.process(std::span(output).subspan(at, std::min(block, total - at)));
        watch_allocations = false;
        require(render_allocations == before, "allocation in audio process");
    }
    return output;
}
void grid_contract() {
    TempoGrid grid(48000, 120000);
    require(grid.next_bar(0) == 0 && grid.next_bar(1) == 96000 && grid.next_bar(96000) == 96000,
            "integral bar policy");
    require(grid.next_bar(96001) == 192000, "bar after exact boundary");
    // Independent closed-form rational oracle, including boundaries rounded up
    // to a sample and positions immediately after them.
    for (auto rate : {44100U, 48000U, 96000U}) {
        for (auto bpm : {123000U, 127123U, 20000U, 300000U}) {
            TempoGrid fractional(rate, bpm);
            const Frame numerator = static_cast<Frame>(rate) * 240000;
            Frame previous = 0;
            for (Frame k = 1; k <= 10000; ++k) {
                const auto boundary = (k * numerator + bpm - 1) / bpm;
                require(fractional.next_bar(previous + 1) == boundary, "fractional next boundary");
                require(fractional.next_bar(boundary) == boundary, "rounded boundary skipped");
                previous = boundary;
            }
        }
    }
    bool overflow = false;
    try { (void)grid.next_bar(std::numeric_limits<Frame>::max()); }
    catch (const std::overflow_error&) { overflow = true; }
    require(overflow, "unrepresentable boundary must reject");
}
void audio_contract() {
    const auto plan = audio_plan();
    const auto bar = TempoGrid(48000, 120000).next_bar(1);
    const std::array commands{Command{CommandKind::launch, bar, 0, 0}, Command{CommandKind::stop_track, bar + 11, 0}};
    for (auto block : {1U, 64U, 128U, 257U, 512U, 4096U}) {
        const auto samples = render(plan, commands, block, 96020);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const float expected = i >= bar && i < bar + 11 ? 0.5F * static_cast<float>((i - bar) % 3 + 1) : 0;
            require(samples[i] == expected, "launch/loop/stop not sample exact");
        }
    }
    auto mix = plan;
    mix.tracks.push_back(Track{0.25F, {AudioClip{{4, 8, 12}, false}}});
    const std::array launches{Command{CommandKind::launch, 0, 0, 0}, Command{CommandKind::launch, 0, 1, 0}};
    const auto samples = render(mix, launches, 257, 8);
    require(samples[0] == 1.5F && samples[1] == 3 && samples[2] == 4.5F && samples[3] == 0.5F,
            "mix or one-shot boundary wrong");
    const std::array last_stop{Command{CommandKind::launch, 0, 0, 0}, Command{CommandKind::stop_track, 0, 0}};
    require(render(plan, last_stop, 64, 2)[0] == 0, "same-frame submission order");
    const std::array last_launch{Command{CommandKind::stop_track, 0, 0}, Command{CommandKind::launch, 0, 0, 0}};
    require(render(plan, last_launch, 64, 2)[0] == 0.5F, "same-frame reverse order");
}
void midi_contract() {
    SessionPlan plan;
    plan.tracks.push_back(Track{1, {MidiClip{257, {{0, 100, 60, 1}, {128, 64, 67, 0.5F}}, true}, AudioClip{{0}, true}}});
    const std::array launch{Command{CommandKind::launch, 0, 0, 0}};
    const auto baseline = render(plan, launch, 1, 1028);
    require(baseline[1] != 0 && baseline[99] != 0 && baseline[100] == 0 && baseline[129] != 0 && baseline[192] == 0,
            "MIDI note duration boundary");
    for (auto block : {64U, 128U, 257U, 512U})
        require(render(plan, launch, block, 1028) == baseline, "MIDI differs by block size");
    for (std::size_t i = 0; i < baseline.size(); ++i)
        require(baseline[i] == baseline[i % 257], "MIDI loop did not restart deterministically");
    const std::array replace{Command{CommandKind::launch, 0, 0, 0}, Command{CommandKind::launch, 20, 0, 1}};
    const auto replaced = render(plan, replace, 257, 300);
    require(std::all_of(replaced.begin() + 20, replaced.end(), [](float x) { return x == 0; }), "replacement leaves voices");
    Engine engine(plan);
    require(engine.schedule(launch[0]) == ScheduleResult::accepted, "MIDI launch rejected");
    engine.start();
    std::array<float, 4> buffer{};
    engine.process(buffer);
    require(engine.active_voices() == 1, "expected live voice");
    const auto at = engine.position();
    engine.stop();
    engine.process(buffer);
    require(engine.active_voices() == 0 && engine.position() == at && buffer[1] == 0, "stop/panic must silence and freeze");
    engine.rewind();
    require(engine.position() == 0, "stopped rewind");
}
void queue_contract() {
    const auto plan = audio_plan();
    Engine engine(plan);
    for (std::size_t i = 0; i < Engine::command_capacity; ++i)
        require(engine.schedule({CommandKind::launch, 0, 0, 0}) == ScheduleResult::accepted, "queue early full");
    require(engine.schedule({CommandKind::stop_track, 0, 0}) == ScheduleResult::full, "overflow must report full");
    engine.stop(); // explicit recovery; rejected command was not silently applied
    engine.start();
    std::array<float, 1> sample{};
    engine.process(sample);
    require(sample[0] == 0, "overflow recovery did not clear pending playback");
    for (std::size_t i = 0; i < 1024; ++i) {
        require(engine.schedule({CommandKind::launch, engine.position(), 0, 0}) == ScheduleResult::accepted, "ring reuse");
        engine.process(sample);
        require(sample[0] == 0.5F, "launch must restart source");
    }
    require(engine.schedule({CommandKind::launch, 0, 0, 0}) == ScheduleResult::in_past, "past command");
    require(engine.schedule({CommandKind::launch, 2000, 0, 0}) == ScheduleResult::accepted, "future command");
    require(engine.schedule({CommandKind::launch, 1999, 0, 0}) == ScheduleResult::out_of_order, "order policy");
    require(engine.schedule({CommandKind::launch, 2000, 7, 0}) == ScheduleResult::invalid, "invalid track");
    require(engine.schedule({CommandKind::launch, 2000, 0, 7}) == ScheduleResult::invalid, "invalid clip");
}
void validation_contract() {
    invalid([] { (void)TempoGrid(0, 120000); });
    invalid([] { (void)TempoGrid(48000, 0); });
    auto plan = audio_plan();
    std::get<AudioClip>(plan.tracks[0].clips[0]).samples[0] = std::numeric_limits<float>::quiet_NaN();
    invalid([&] { Engine engine(plan); });
    plan.tracks[0].clips[0] = MidiClip{10, {{0, 11, 60, 1}}, true};
    invalid([&] { Engine engine(plan); });
    plan.tracks[0].clips[0] = MidiClip{10, {{2, 1, 60, 1}, {1, 1, 60, 1}}, true};
    invalid([&] { Engine engine(plan); });
    plan.tracks[0].clips[0] = MidiClip{10, std::vector<Note>(17, Note{0, 10, 60, 1}), true};
    invalid([&] { Engine engine(plan); });
    plan.tracks[0].clips[0] = AudioClip{};
    invalid([&] { Engine engine(plan); });
    plan.tracks[0].clips[0] = AudioClip{{1, 2, 3}, true, 2};
    invalid([&] { Engine engine(plan); });
    for (auto channels : {0U, 3U}) {
        plan.tracks[0].clips[0] = AudioClip{{1, 2, 3, 4, 5, 6}, true, channels};
        invalid([&] { Engine engine(plan); });
    }
    plan.tracks[0].clips[0] = AudioClip{{1}, true};
    for (auto pan : {-1.01F, 1.01F, std::numeric_limits<float>::quiet_NaN()}) {
        plan.tracks[0].pan = pan;
        invalid([&] { Engine engine(plan); });
    }
}
std::vector<float> render_channels(const SessionPlan& plan, std::span<const Command> commands,
                                   std::size_t block_frames, std::size_t frames, std::size_t channels) {
    Engine engine(plan);
    for (auto c : commands) require(engine.schedule(c) == ScheduleResult::accepted, "schedule failed");
    std::vector<float> output(frames * channels);
    engine.start();
    for (std::size_t at = 0; at < frames; at += block_frames) {
        const auto count = std::min(block_frames, frames - at);
        const auto before = render_allocations;
        watch_allocations = true;
        const bool rendered = engine.process(std::span(output).subspan(at * channels, count * channels), channels);
        watch_allocations = false;
        require(rendered, "supported layout refused");
        require(render_allocations == before, "allocation in multichannel process");
    }
    require(engine.position() == frames, "multichannel playhead must advance by frames");
    return output;
}
void stereo_contract() {
    // Track 0: interleaved stereo source at center. Track 1: mono source panned
    // half right, so only its left channel is attenuated (balance law).
    SessionPlan plan;
    plan.tracks.push_back(Track{0.5F, {AudioClip{{1, -1, 2, -2, 3, -3}, true, 2}}});
    plan.tracks.push_back(Track{0.25F, {AudioClip{{4, 8}, true}, MidiClip{96, {{0, 48, 69, 1}}, true}}, 0.5F});
    const std::array commands{Command{CommandKind::launch, 7, 0, 0}, Command{CommandKind::launch, 7, 1, 0},
        Command{CommandKind::launch, 300, 1, 1}, Command{CommandKind::stop_track, 401, 0}, Command{CommandKind::stop_track, 401, 1}};
    constexpr std::size_t frames = 512;
    const auto baseline = render_channels(plan, commands, 1, frames, 2);
    for (std::size_t i = 0; i < 300; ++i) {
        const auto age = i - 7;
        const float left = i < 7 ? 0 : 0.5F * static_cast<float>(age % 3 + 1) + 0.125F * (age % 2 ? 8.0F : 4.0F);
        const float right = i < 7 ? 0 : -0.5F * static_cast<float>(age % 3 + 1) + 0.25F * (age % 2 ? 8.0F : 4.0F);
        require(baseline[2 * i] == left && baseline[2 * i + 1] == right, "stereo routing or pan law wrong");
    }
    for (std::size_t i = 300; i < 401; ++i) {
        // MIDI replacement on track 1: both channels carry the instrument, the
        // left one at half level; track 0 keeps its stereo source.
        const auto age = i - 7;
        const float source_left = 0.5F * static_cast<float>(age % 3 + 1), source_right = -source_left;
        const float midi_left = baseline[2 * i] - source_left, midi_right = baseline[2 * i + 1] - source_right;
        require(std::abs(midi_left - midi_right / 2) < 1e-6F, "MIDI instrument pan mismatch");
        if (i - 300 >= 1 && i - 300 < 48) require(midi_right != 0, "MIDI note missing from right channel");
        if (i - 300 >= 48 && i - 300 < 96) require(std::abs(midi_right) < 1e-6F, "MIDI note outlived its duration");
    }
    require(std::all_of(baseline.begin() + 2 * 401, baseline.end(), [](float x) { return x == 0; }), "stereo stop left output");
    for (auto block : {64U, 128U, 257U, 512U})
        require(render_channels(plan, commands, block, frames, 2) == baseline, "stereo output differs by block size");
    for (auto block : {1U, 257U}) {
        const auto mono = render_channels(plan, commands, block, frames, 1);
        for (std::size_t i = 0; i < frames; ++i)
            require(mono[i] == (baseline[2 * i] + baseline[2 * i + 1]) / 2, "mono is not the declared downmix");
    }
    auto hard = plan;
    hard.tracks[0].pan = -1;
    hard.tracks[1].pan = 1;
    const auto extremes = render_channels(hard, std::span(commands).first(2), 257, 64, 2);
    for (std::size_t i = 7; i < 64; ++i) {
        const auto age = i - 7;
        require(extremes[2 * i] == 0.5F * static_cast<float>(age % 3 + 1) && extremes[2 * i + 1] == 0.25F * (age % 2 ? 8.0F : 4.0F),
                "hard pan must silence only the far channel");
    }
    // Unsupported layouts fail safely: silence, no command application, no advance.
    Engine engine(plan);
    require(engine.schedule(commands[0]) == ScheduleResult::accepted, "schedule failed");
    engine.start();
    std::array<float, 9> odd{};
    odd.fill(7);
    require(!engine.process(std::span(odd).first(5), 2) && engine.position() == 0, "partial stereo frame accepted");
    require(!engine.process(odd, 3) && engine.position() == 0, "unsupported channel count accepted");
    require(std::all_of(odd.begin(), odd.end(), [](float x) { return x == 0; }), "refused layout must be silent");
    RenderController controller(plan);
    (void)controller.submit({ControlKind::start, {}, 1});
    std::array<float, 6> out{};
    controller.process(out, 3);
    require(controller.status().handled_serial == 0 && !controller.status().running, "refused layout consumed controls");
    controller.process(out, 2);
    require(controller.status().handled_serial == 1 && controller.status().position == 3, "stereo controller frames");
}
// Independent statement of the declared policy: linear in frames between
// points, end values held outside them.
float expected_automation(std::initializer_list<std::pair<Frame, float>> points, Frame f) {
    const std::vector<std::pair<Frame, float>> v(points);
    if (f <= v.front().first) return v.front().second;
    if (f >= v.back().first) return v.back().second;
    for (std::size_t i = 0; i + 1 < v.size(); ++i)
        if (f < v[i + 1].first) {
            const double t = static_cast<double>(f - v[i].first) / static_cast<double>(v[i + 1].first - v[i].first);
            return static_cast<float>(v[i].second + (static_cast<double>(v[i + 1].second) - v[i].second) * t);
        }
    return v.back().second;
}
void automation_contract() {
    SessionPlan plan;
    plan.tracks.push_back(Track{1, {AudioClip{{1}, true}}});
    plan.tracks[0].gain_automation = {{100, 0.0F}, {1100, 1.0F}, {2000, 0.5F}};
    plan.tracks[0].pan_automation = {{0, -1.0F}, {3000, 1.0F}};
    const std::array launch{Command{CommandKind::launch, 0, 0, 0}};
    const std::size_t frames = 3500;
    const auto reference = render_channels(plan, launch, 1, frames, 2);
    for (auto block : {64U, 128U, 257U, 512U}) require(render_channels(plan, launch, block, frames, 2) == reference, "automation differs by block size");
    for (Frame f = 0; f < frames; ++f) {
        const float gain = expected_automation({{100, 0.0F}, {1100, 1.0F}, {2000, 0.5F}}, f);
        const float pan = expected_automation({{0, -1.0F}, {3000, 1.0F}}, f);
        const float left = gain * (pan > 0 ? 1 - pan : 1.0F), right = gain * (pan < 0 ? 1 + pan : 1.0F);
        require(reference[2 * f] == left && reference[2 * f + 1] == right, "automated sample differs from the declared interpolation");
    }
    // Rewind restarts the curve from the beginning (cursor reset).
    Engine engine(plan);
    require(engine.schedule(launch[0]) == ScheduleResult::accepted, "launch");
    engine.start();
    std::vector<float> block(2 * 1500);
    require(engine.process(block, 2), "process");
    engine.stop(); engine.rewind();
    require(engine.schedule(launch[0]) == ScheduleResult::accepted, "relaunch");
    engine.start();
    std::vector<float> again(2 * 1500);
    require(engine.process(again, 2) && again == std::vector<float>(reference.begin(), reference.begin() + 3000), "rewind must restart automation");
    // Validation: order, range, NaN, capacity.
    auto bad = plan;
    bad.tracks[0].gain_automation = {{10, 0.5F}, {10, 0.6F}}; invalid([&] { Engine e(bad); });
    bad.tracks[0].gain_automation = {{10, 4.5F}}; invalid([&] { Engine e(bad); });
    bad.tracks[0].gain_automation = {{10, std::numeric_limits<float>::quiet_NaN()}}; invalid([&] { Engine e(bad); });
    bad.tracks[0].gain_automation = {}; bad.tracks[0].pan_automation = {{0, -1.5F}}; invalid([&] { Engine e(bad); });
    bad.tracks[0].pan_automation.assign(Engine::automation_points_capacity + 1, AutomationPoint{0, 0});
    for (std::size_t i = 0; i < bad.tracks[0].pan_automation.size(); ++i) bad.tracks[0].pan_automation[i].at = i;
    invalid([&] { Engine e(bad); });
}
// Independent statement of the declared envelope (linear A/D/S/R in frames).
double expected_envelope(Frame a, Frame d, double sustain, Frame r, Frame held, Frame t) {
    const auto hold = [&](Frame x) { return x < a ? static_cast<double>(static_cast<float>(x) / static_cast<float>(a))
        : x < a + d ? 1 - (1 - sustain) * static_cast<double>(x - a) / static_cast<double>(d) : sustain; };
    if (t < held) return hold(t);
    if (r == 0 || t >= held + r) return 0;
    return hold(held) * (1 - static_cast<double>(t - held) / static_cast<double>(r));
}
void instrument_contract() {
    // One A4 note, velocity 0.8, held 4000 frames, in a 12000-frame clip.
    SessionPlan plan;
    Instrument in{Waveform::sine, 480, 960, 0.5F, 1200};
    plan.tracks.push_back(Track{1, {MidiClip{12000, {{0, 4000, 69, 0.8F}}, true, in}}});
    const std::array launch{Command{CommandKind::launch, 0, 0, 0}};
    const auto reference = render_channels(plan, launch, 1, 6000, 1);
    for (auto block : {64U, 257U, 512U}) require(render_channels(plan, launch, block, 6000, 1) == reference, "instrument differs by block size");
    double worst = 0;
    for (Frame t = 0; t < 6000; ++t) {
        const double env = expected_envelope(480, 960, 0.5, 1200, 4000, t);
        const double expected = std::sin(2 * std::numbers::pi * 440.0 / 48000.0 * static_cast<double>(t)) * 0.08 * env;
        worst = std::max(worst, std::abs(reference[t] - expected));
    }
    require(worst < 2e-6, "envelope or pitch differs from the independent fixture");
    require(reference[5200] == 0 && reference[5999] == 0, "release must end at note end plus release");
    // Velocity scales linearly; waveforms follow their declared shapes.
    auto quiet = plan; std::get<MidiClip>(quiet.tracks[0].clips[0]).notes[0].velocity = 0.4F;
    const auto half = render_channels(quiet, launch, 257, 6000, 1);
    for (Frame t = 0; t < 6000; t += 37) require(std::abs(half[t] - reference[t] / 2) < 1e-7, "velocity must scale linearly");
    for (auto [wave, at, value] : {std::tuple{Waveform::square, Frame{10}, 0.08F}, std::tuple{Waveform::square, Frame{70}, -0.08F},
                                   std::tuple{Waveform::saw, Frame{0}, -0.08F}, std::tuple{Waveform::triangle, Frame{0}, 0.08F}}) {
        auto shaped = plan; auto& clip = std::get<MidiClip>(shaped.tracks[0].clips[0]);
        clip.instrument = Instrument{wave, 0, 0, 1, 0};
        const auto out = render_channels(shaped, launch, 64, 200, 1);
        require(std::abs(out[at] - value) < 1e-6, "waveform shape");
    }
    // A release crosses the loop boundary instead of being cut.
    auto looping = plan; auto& clip = std::get<MidiClip>(looping.tracks[0].clips[0]);
    clip.length = 4500; clip.notes[0].duration = 4400;
    const auto wrap = render_channels(looping, launch, 257, 4700, 1);
    require(wrap[4550] != 0, "release tail must continue across the loop boundary");
    // Overload: 16 sustained notes plus releasing ones; a new note steals the
    // quietest releasing voice, never more than 16 voices sound.
    SessionPlan busy;
    MidiClip crowd{20000, {}, true, Instrument{Waveform::sine, 0, 0, 1, 8000}};
    for (int n = 0; n < 16; ++n) crowd.notes.push_back({static_cast<Frame>(n), 1000, static_cast<std::uint8_t>(40 + n), 1});
    crowd.notes.push_back({2000, 100, 90, 1});
    busy.tracks.push_back(Track{1, {crowd}});
    Engine engine(busy);
    require(engine.schedule(launch[0]) == ScheduleResult::accepted, "launch");
    engine.start();
    std::vector<float> out(2048);
    require(engine.process(out, 1) && engine.active_voices() == 16, "sixteen voices sound, the extra note stole one");
    // Stop and replacement still cut every voice at once.
    engine.stop(); require(engine.active_voices() == 0, "stop must clear releasing voices");
    // Refusals.
    auto bad = plan; std::get<MidiClip>(bad.tracks[0].clips[0]).instrument.sustain = 1.5F; invalid([&] { Engine e(bad); });
    std::get<MidiClip>(bad.tracks[0].clips[0]).instrument = Instrument{Waveform::sine, Engine::envelope_frame_limit + 1}; invalid([&] { Engine e(bad); });
    std::get<MidiClip>(bad.tracks[0].clips[0]).instrument = Instrument{static_cast<Waveform>(9)}; invalid([&] { Engine e(bad); });
}
void controller_allocation_contract() {
    const auto plan = audio_plan();
    RenderController controller(plan);
    std::array<float, 257> output{};
    Control scene{ControlKind::scene,{},5};scene.batch.count=1;
    scene.batch.commands[0]={CommandKind::launch,0,0,0};
    const auto before = render_allocations;
    watch_allocations = true;
    (void)controller.submit({ControlKind::start, {}, 1});
    (void)controller.submit({ControlKind::schedule, {CommandKind::launch, 0, 0, 0}, 2});
    controller.process(output);
    (void)controller.submit({ControlKind::launch_next_bar, {CommandKind::launch, 0, 0, 0}, 3});
    (void)controller.submit({ControlKind::stop_next_bar, {CommandKind::stop_track, 0, 0, 0}, 4});
    controller.process(output);
    (void)controller.submit(scene);controller.process(output);
    scene.policy=LaunchPolicy::immediate;scene.serial=6;(void)controller.submit(scene);controller.process(output);
    (void)TempoGrid(48000,120000).try_next_bar(std::numeric_limits<Frame>::max());
    for (std::uint64_t n = 7; n < 71; ++n) (void)controller.submit({ControlKind::start, {}, n});
    controller.panic();
    controller.process(output);
    controller.finish();
    watch_allocations = false;
    require(render_allocations == before, "allocation in control handoff/process/panic");
    require(!controller.status().running, "finish must acknowledge stopped state");
}
}
int main() {
    try {
        grid_contract(); audio_contract(); midi_contract(); stereo_contract(); automation_contract(); instrument_contract(); queue_contract(); validation_contract();
        controller_allocation_contract();
        std::cout << "PASS: grid, audio, MIDI, stereo/pan, automation, instrument envelopes and voice stealing, command bounds, validation; process allocations=" << render_allocations << '\n';
        return 0;
    } catch (const std::exception& e) {
        watch_allocations = false;
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
