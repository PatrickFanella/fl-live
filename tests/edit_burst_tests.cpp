// Panic and backpressure under playback edit bursts (#55): a saturated
// control queue refuses controls and replacements explicitly, panic silences
// within one render block even when the queue is full or a replacement holds
// later controls, and the rendering thread neither allocates nor frees.
#include "fl_live/controller.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local bool watching = false;
std::atomic<std::size_t> callback_allocations{0}, callback_releases{0};
void allocated() { if (watching) callback_allocations.fetch_add(1); }
void released(void* p) { if (watching && p) callback_releases.fetch_add(1); std::free(p); }
}
void* operator new(std::size_t size) {
    allocated();
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { released(p); }
void operator delete[](void* p) noexcept { released(p); }
void operator delete(void* p, std::size_t) noexcept { released(p); }
void operator delete[](void* p, std::size_t) noexcept { released(p); }

namespace {
using namespace fl_live;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
AudioClip constant(float value) { return AudioClip{std::vector<float>(4, value), true}; }
SessionPlan plan(std::size_t tracks, float value) {
    SessionPlan result{48000, 120000, {}};
    for (std::size_t t = 0; t < tracks; ++t) result.tracks.push_back({1, {constant(value)}});
    return result;
}
PlanMapping identity(std::size_t from, std::size_t to) {
    PlanMapping mapping;
    for (std::size_t t = 0; t < from; ++t) {
        mapping.track_from_old.push_back(t < to ? static_cast<std::int32_t>(t) : -1);
        mapping.clip_from_old.push_back({t < to ? 0 : -1});
    }
    return mapping;
}
std::unique_ptr<PlanReplacement> replacement(SessionPlan next, PlanMapping mapping, Frame at = 0) {
    return std::make_unique<PlanReplacement>(PlanReplacement{std::make_shared<const SessionPlan>(std::move(next)), std::move(mapping), at});
}
bool silent(std::span<const float> block) { return std::all_of(block.begin(), block.end(), [](float v) { return v == 0; }); }

void saturationAndRecovery() {
    const auto initial = plan(2, 0.25F);
    RenderController controller(initial);
    std::array<float, 64> out{};
    std::uint64_t serial = 0;
    require(controller.submit({ControlKind::start, {}, ++serial}), "start");
    require(controller.submit({ControlKind::schedule, {CommandKind::launch, 0, 0, 0}, ++serial}), "launch 0");
    require(controller.submit({ControlKind::schedule, {CommandKind::launch, 0, 1, 0}, ++serial}), "launch 1");
    controller.process(out);
    require(out[0] == 0.5F, "both tracks playing");

    // Saturate: controls are refused at capacity, and so is a replacement.
    std::size_t queued = 0;
    while (controller.submit({ControlKind::launch_next_bar, {CommandKind::launch, 0, 0, 0}, serial + 1})) { ++serial; ++queued; }
    require(queued == Engine::command_capacity, "queue accepts exactly its capacity");
    require(controller.offer(replacement(plan(2, 0.125F), identity(2, 2))) == OfferResult::busy, "saturated queue refuses the replacement");
    require(!controller.submit({ControlKind::stop, {}, serial + 1}), "saturated queue refuses ordinary stop");
    // Panic needs no queue slot: silence in the next block, queue discarded.
    controller.panic();
    controller.process(out);
    auto status = controller.status();
    require(silent(out) && !status.running && status.handled_serial == serial && status.generation == 0, "panic under saturation");
    controller.process(out);
    require(silent(out), "stays silent");
    // Recovery: a replacement and fresh controls are accepted and apply.
    require(controller.offer(replacement(plan(2, 0.125F), identity(2, 2))) == OfferResult::accepted, "offer after panic");
    const auto resume = controller.status().position;
    require(controller.submit({ControlKind::start, {}, ++serial}) && controller.submit({ControlKind::schedule, {CommandKind::launch, resume, 0, 0}, ++serial}), "restart");
    controller.process(out);
    status = controller.status();
    require(status.running && status.generation == 1 && status.handled_serial == serial && out[0] == 0.125F, "recovered under the new plan");
    require(controller.collect().acknowledged, "retirement collected");

    // A held replacement (far-future frame) delays later controls, including
    // stop, but never panic.
    require(controller.offer(replacement(plan(2, 0.0625F), identity(2, 2), std::numeric_limits<Frame>::max())) == OfferResult::accepted, "held offer");
    require(controller.submit({ControlKind::stop, {}, ++serial}), "stop behind the held replacement");
    controller.process(out);
    status = controller.status();
    require(status.running && status.handled_serial == serial - 1 && out[0] == 0.125F, "stop waits behind the held replacement");
    controller.panic();
    controller.process(out);
    status = controller.status();
    require(silent(out) && !status.running && status.generation == 2 && status.handled_serial == serial, "panic activates the held replacement and silences");
    require(controller.collect().acknowledged, "held retirement collected");
}

// A producer floods controls and replacements, sometimes with a full queue,
// and requests panic periodically. The render thread measures how many blocks
// pass between a panic request and a silent, stopped block.
struct BurstResult { std::size_t panics, max_latency, refused_controls, busy_offers, accepted_offers; double max_block_us; };
BurstResult burst() {
    const auto initial = plan(4, 1.0F / 64);
    RenderController controller(initial);
    std::atomic<bool> done{false}, bad{false};
    std::atomic<std::uint64_t> blocks{0};
    std::atomic<std::uint64_t> panic_at{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::size_t> max_latency{0};
    double max_block_us = 0;
    std::thread render([&] {
        watching = true;
        std::array<float, 128> out{};
        while (!done.load()) {
            const auto start = std::chrono::steady_clock::now();
            controller.process(out, 2);
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            max_block_us = std::max(max_block_us, us);
            const auto block = blocks.fetch_add(1) + 1;
            for (const auto v : out) if (!std::isfinite(v) || std::abs(v) > 1) bad.store(true);
            const auto requested = panic_at.load();
            if (requested != std::numeric_limits<std::uint64_t>::max() && !controller.status().running) {
                if (!silent(out)) bad.store(true);
                std::size_t latency = static_cast<std::size_t>(block - requested), seen = max_latency.load();
                while (latency > seen && !max_latency.compare_exchange_weak(seen, latency)) {}
                panic_at.store(std::numeric_limits<std::uint64_t>::max());
            }
        }
        watching = false;
    });
    std::size_t tracks = 4, panics = 0, refused = 0, busy = 0, accepted = 0;
    std::uint64_t serial = 0;
    for (std::size_t round = 1; round <= 3000 && !bad.load(); ++round) {
        while (controller.collect().acknowledged) {}
        const auto next = 1 + round % 8;
        const auto result = controller.offer(replacement(plan(next, static_cast<float>(round % 5 + 1) / 64), identity(tracks, next)));
        if (result == OfferResult::accepted) { ++accepted; tracks = next; }
        else if (result == OfferResult::busy) ++busy;
        else bad.store(true);
        // A burst of controls, often more than the queue holds.
        const auto count = round % 7 == 0 ? Engine::command_capacity + 8 : round % 13;
        for (std::size_t n = 0; n < count; ++n) {
            Control control{ControlKind::start, {}, serial + 1};
            if (n % 3 == 1) control = {ControlKind::launch_next_bar, {CommandKind::launch, 0, n % tracks, 0}, serial + 1};
            if (n % 3 == 2) {
                CommandBatch batch;
                for (std::size_t t = 0; t < tracks; ++t) batch.commands[batch.count++] = {CommandKind::launch, 0, t, 0};
                control = {ControlKind::scene, {}, serial + 1, batch, LaunchPolicy::immediate};
            }
            if (controller.submit(control)) ++serial; else ++refused;
        }
        if (round % 50 == 0) {
            // Count from after the request: at most the block already in
            // progress misses it, and the next one renders the panic.
            controller.panic();
            panic_at.store(blocks.load());
            ++panics;
            while (panic_at.load() != std::numeric_limits<std::uint64_t>::max() && !bad.load()) std::this_thread::yield();
        }
    }
    done.store(true);
    render.join();
    controller.finish();
    require(!bad.load(), "outputs finite and bounded; panic blocks silent; no invalid offers");
    return {panics, max_latency.load(), refused, busy, accepted, max_block_us};
}

// Worst-case replacement on one block: 64 MIDI tracks of 1024 notes with
// sounding voices, and a full command queue to remap.
double worstReplacementMicros() {
    const auto midi = [](std::uint8_t pitch) {
        MidiClip clip{1024 * 64 + 640, {}, true};
        for (Frame n = 0; n < 1024; ++n) clip.notes.push_back({n * 64, 640, static_cast<std::uint8_t>(pitch + n % 12), 0.5F});
        return clip;
    };
    SessionPlan a{48000, 120000, {}}, b{48000, 120000, {}};
    for (std::size_t t = 0; t < Engine::track_capacity; ++t) {
        a.tracks.push_back({0.01F, {midi(48)}});
        b.tracks.push_back({0.01F, {midi(50)}});
    }
    PlanMapping reversed;
    for (std::size_t t = 0; t < Engine::track_capacity; ++t) {
        reversed.track_from_old.push_back(static_cast<std::int32_t>(Engine::track_capacity - 1 - t));
        reversed.clip_from_old.push_back({0});
    }
    Engine engine(a);
    for (std::size_t t = 0; t < Engine::track_capacity; ++t) require(engine.schedule({CommandKind::launch, 0, t, 0}) == ScheduleResult::accepted, "launch");
    engine.start();
    std::vector<float> out(2 * 4096);
    (void)engine.process(out, 2);
    for (std::size_t n = 0; n < Engine::command_capacity; ++n)
        require(engine.schedule({CommandKind::stop_track, 1U << 20, n % Engine::track_capacity, 0}) == ScheduleResult::accepted, "queue");
    watching = true;
    const auto start = std::chrono::steady_clock::now();
    const auto result = engine.replace(b, reversed);
    const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    (void)engine.process(out, 2);
    watching = false;
    require(result.dropped_commands == 0 && result.stopped_tracks == 0 && engine.active_voices() > 0, "worst-case replacement keeps every track sounding");
    return us;
}
}

int main() {
    try {
        saturationAndRecovery();
        const auto result = burst();
        const auto replace_us = worstReplacementMicros();
        require(result.max_latency <= 2, "panic silences within two render blocks of the request");
        // A full queue refuses controls deterministically in saturationAndRecovery;
        // here a fast render thread can drain a burst before it fills, so
        // saturation shows as refused controls or busy replacements.
        if (!(result.refused_controls + result.busy_offers > 0 && result.accepted_offers > 0 && result.panics == 60))
            throw std::runtime_error("saturation was not exercised: refused " + std::to_string(result.refused_controls) + ", busy " +
                std::to_string(result.busy_offers) + ", accepted " + std::to_string(result.accepted_offers) + ", panics " + std::to_string(result.panics));
        require(callback_allocations.load() == 0 && callback_releases.load() == 0, "render thread neither allocates nor frees");
        std::cout << "PASS: saturation refuses controls and replacements explicitly; panic needs no queue slot and passes a held replacement; "
                  << result.panics << " panics under bursts silenced within " << result.max_latency << " render block(s); "
                  << result.refused_controls << " refused controls, " << result.busy_offers << " busy and " << result.accepted_offers
                  << " accepted replacements; no render-thread allocation or release\n"
                  << "MEASURED (host-dependent, not a deadline): max 64-frame stereo block " << result.max_block_us
                  << " us under bursts; worst-case 64x1024-note replacement " << replace_us << " us\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
