#include "fl_live/engine.hpp"
#include "fl_live/controller.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
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
        grid_contract(); audio_contract(); midi_contract(); queue_contract(); validation_contract(); controller_allocation_contract();
        std::cout << "PASS: grid, audio, MIDI, command bounds, validation; process allocations=" << render_allocations << '\n';
        return 0;
    } catch (const std::exception& e) {
        watch_allocations = false;
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
