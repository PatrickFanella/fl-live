// Immutable plan replacement with acknowledged retirement (#53): activation
// boundary, command remapping, control ordering, refusals, MIDI voice
// continuity, and a threaded replace/retire/shutdown stress run. Retired
// plans are poisoned with NaN before destruction, so a stale render-side
// reference shows up in the output as well as under AddressSanitizer.
#include "fl_live/controller.hpp"
#include <algorithm>
#include <atomic>
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
AudioClip constant(float value, std::size_t frames = 4) { return AudioClip{std::vector<float>(frames, value), true}; }
std::unique_ptr<PlanReplacement> replacement(SessionPlan plan, PlanMapping mapping, Frame at = 0) {
    return std::make_unique<PlanReplacement>(PlanReplacement{std::make_shared<const SessionPlan>(std::move(plan)), std::move(mapping), at});
}
Control schedule(Command command, std::uint64_t serial) { return {ControlKind::schedule, command, serial}; }

// Old track 0 is deleted, old track 1 becomes new track 0 with new content.
// The swap is due at frame 1000; a queued launch on the deleted track is
// dropped and a queued stop on the surviving track follows it.
std::vector<float> renderSwap(std::size_t block, RenderStatus& final, Retirement& ack) {
    const SessionPlan a{48000, 120000, {{1, {constant(0.25F), constant(0.0625F)}}, {1, {constant(0.5F)}}}};
    RenderController controller(a);
    require(controller.submit({ControlKind::start, {}, 1}), "start");
    require(controller.submit(schedule({CommandKind::launch, 0, 0, 0}, 2)), "launch 0");
    require(controller.submit(schedule({CommandKind::launch, 0, 1, 0}, 3)), "launch 1");
    require(controller.submit(schedule({CommandKind::launch, 1500, 0, 1}, 4)), "launch on deleted track");
    require(controller.submit(schedule({CommandKind::stop_track, 2000, 1, 0}, 5)), "stop survivor");
    controller.process(std::span<float>{}); // apply controls without rendering
    SessionPlan b{48000, 120000, {{1, {constant(0.125F)}}}};
    require(controller.offer(replacement(std::move(b), {{-1, 0}, {{-1, -1}, {0}}}, 1000)) == OfferResult::accepted, "offer");
    std::vector<float> out(3000);
    for (std::size_t at = 0; at < out.size(); at += block) controller.process(std::span(out).subspan(at, std::min(block, out.size() - at)));
    final = controller.status();
    ack = controller.collect();
    require(!controller.collect().acknowledged, "one acknowledgement per activation");
    return out;
}
void activationBoundary() {
    std::vector<float> expected(3000);
    for (std::size_t f = 0; f < expected.size(); ++f) expected[f] = f < 1000 ? 0.75F : f < 2000 ? 0.125F : 0.0F;
    for (const auto block : {1U, 64U, 257U, 512U, 3000U}) {
        RenderStatus status{};
        Retirement ack;
        require(renderSwap(block, status, ack) == expected, "replacement activates exactly at frame 1000 for every block size");
        require(status.generation == 1 && status.dropped_commands == 1 && status.track_count == 1 && status.rejected == 0,
            "one activation, one dropped command, remapped status");
        require(ack.acknowledged && !ack.retired, "the constructor's plan retires without transferring ownership");
    }
}

void orderingAndRefusals() {
    const SessionPlan a{48000, 120000, {{1, {constant(0.25F)}}, {1, {constant(0.5F)}}}};
    RenderController controller(a);
    const PlanMapping identity{{0, 1}, {{0}, {0}}};
    const auto two = [](float value) { return SessionPlan{48000, 120000, {{1, {constant(value)}}, {1, {constant(value)}}}}; };
    require(controller.offer(nullptr) == OfferResult::invalid, "null offer");
    require(controller.offer(replacement({44100, 120000, two(0.1F).tracks}, identity)) == OfferResult::invalid, "sample rate change");
    require(controller.offer(replacement({48000, 90000, two(0.1F).tracks}, identity)) == OfferResult::invalid, "tempo change");
    require(controller.offer(replacement(two(0.1F), {{0}, {{0}}})) == OfferResult::invalid, "mapping misses a track");
    require(controller.offer(replacement(two(0.1F), {{0, 1}, {{0}, {}}})) == OfferResult::invalid, "mapping misses a clip");
    require(controller.offer(replacement(two(0.1F), {{0, 0}, {{0}, {0}}})) == OfferResult::invalid, "two old tracks share one new track");
    require(controller.offer(replacement(two(0.1F), {{0, 2}, {{0}, {0}}})) == OfferResult::invalid, "mapped track out of range");
    require(controller.offer(replacement(two(0.1F), {{0, 1}, {{1}, {0}}})) == OfferResult::invalid, "mapped clip out of range");
    auto loud = two(0.1F); loud.tracks[0].gain = 5;
    require(controller.offer(replacement(std::move(loud), identity)) == OfferResult::invalid, "plan the engine refuses");
    require(!controller.submit({ControlKind::replace}), "the replacement marker is internal");

    // A full control queue refuses the offer and leaves no marker behind.
    std::uint64_t serial = 0;
    while (controller.submit({ControlKind::rewind, {}, ++serial})) {}
    require(controller.offer(replacement(two(0.1F), identity)) == OfferResult::busy, "full queue");
    std::array<float, 64> out{};
    controller.process(out);
    require(controller.status().generation == 0 && !controller.collect().acknowledged, "refusals change nothing");

    require(controller.submit({ControlKind::start, {}, ++serial}), "start");
    controller.process(std::span<float>{});
    // Submitted before the offer: old indexes. After it: new indexes, and it
    // waits behind the replacement, which is due at frame 100.
    require(controller.submit(schedule({CommandKind::launch, 50, 0, 0}, ++serial)), "launch under the old plan");
    require(controller.offer(replacement(two(0.1F), identity, 100)) == OfferResult::accepted, "offer");
    require(controller.offer(replacement(two(0.2F), identity)) == OfferResult::busy, "one offer in flight");
    const auto before = serial;
    require(controller.submit(schedule({CommandKind::launch, 128, 1, 0}, ++serial)), "launch under the new plan");
    controller.process(out); // frames 0..63
    auto status = controller.status();
    require(status.generation == 0 && status.handled_serial == before && status.tracks[0].active_clip == 0 &&
        status.tracks[1].pending == PendingAction::none, "the replacement holds later controls until its frame");
    require(!controller.collect().acknowledged && controller.offer(replacement(two(0.2F), identity)) == OfferResult::busy, "not yet active");
    controller.process(out); // frames 64..127: activates at 100
    status = controller.status();
    require(status.generation == 1 && status.handled_serial == before && out[35] == 0.25F && out[36] == 0.1F,
        "activation exactly at frame 100 with the new content");
    require(controller.offer(replacement(two(0.2F), identity)) == OfferResult::busy, "retirement not yet collected");
    controller.process(out); // frames 128..191: the waiting launch applies
    status = controller.status();
    require(status.handled_serial == serial && status.tracks[1].active_clip == 0 && out[0] == 0.2F && status.rejected == 0,
        "the control behind the replacement uses the new plan");
    auto first = controller.collect();
    require(first.acknowledged && !first.retired, "first retirement is the constructor's plan");
    // While stopped a replacement activates at once, even with a future frame.
    require(controller.submit({ControlKind::stop, {}, ++serial}), "stop");
    require(controller.offer(replacement(two(0.3F), identity, std::numeric_limits<Frame>::max())) == OfferResult::accepted, "stopped offer");
    require(controller.submit({ControlKind::start, {}, ++serial}), "restart");
    require(controller.submit(schedule({CommandKind::launch, 192, 0, 0}, ++serial)), "relaunch");
    controller.process(out);
    status = controller.status();
    auto second = controller.collect();
    require(status.generation == 2 && status.running && status.handled_serial == serial && out[0] == 0.3F, "stopped activation, then the queued controls under the new plan");
    require(second.acknowledged && second.retired && second.retired->plan->tracks.size() == 2 &&
        std::get<AudioClip>(second.retired->plan->tracks[0].clips[0]).samples[0] == 0.1F, "second retirement hands back the first replacement");
}

void midiContinuity() {
    const auto midi = [](Waveform waveform, Frame release, std::vector<Note> notes) {
        MidiClip clip{48000, std::move(notes), false};
        clip.instrument.waveform = waveform;
        clip.instrument.release = release;
        return SessionPlan{48000, 120000, {{1, {clip}}}};
    };
    const PlanMapping keep{{0}, {{0}}}, drop{{0}, {{-1}}};
    const auto a = midi(Waveform::sine, 0, {{0, 24000, 69, 1}});
    // Same held note under a saw; a passed note is not retriggered and a
    // later note still starts.
    const auto b = midi(Waveform::saw, 0, {{0, 24000, 69, 1}, {500, 100, 72, 1}, {1500, 100, 76, 1}});
    const auto c = midi(Waveform::sine, 0, {{0, 24000, 69, 1}});
    Engine engine(a);
    require(engine.schedule({CommandKind::launch, 0, 0, 0}) == ScheduleResult::accepted, "launch MIDI");
    engine.start();
    std::vector<float> out(1000);
    engine.process(out);
    watching = true;
    auto result = engine.replace(b, keep);
    const bool continued = engine.active_voices() == 1;
    engine.process(std::span(out).first(550)); // frames 1000..1549
    const bool later = engine.active_voices() == 2; // a cursor left before 1500 would block that note
    double phase = 0;
    const double step = 440.0 / 48000;
    for (int n = 0; n < 1000; ++n) { phase += step; phase -= std::floor(phase); }
    const bool saw = std::abs(out[0] - static_cast<float>(2 * phase - 1) * 0.1F) < 1e-6F;
    result = engine.replace(c, drop);
    const bool stopped = result.stopped_tracks == 1 && engine.active_voices() == 0 && engine.track_statuses()[0].active_clip < 0;
    engine.process(std::span(out).first(10));
    watching = false;
    require(continued && later && saw, "a held voice keeps sounding under the new instrument; only later notes start");
    require(stopped && std::all_of(out.begin(), out.begin() + 10, [](float v) { return v == 0; }), "a removed clip stops its voices");

    // A releasing voice ends when the new release is shorter.
    const auto slow = midi(Waveform::sine, 4800, {{0, 100, 69, 1}});
    const auto abrupt = midi(Waveform::sine, 0, {{0, 100, 69, 1}});
    Engine released(slow);
    require(released.schedule({CommandKind::launch, 0, 0, 0}) == ScheduleResult::accepted, "launch slow release");
    released.start();
    released.process(std::span(out).first(200));
    require(released.active_voices() == 1, "releasing");
    watching = true;
    (void)released.replace(abrupt, keep);
    released.process(std::span(out).first(100));
    watching = false;
    require(released.active_voices() == 0 && std::all_of(out.begin(), out.begin() + 100, [](float v) { return std::isfinite(v); }),
        "a zero release ends the voice without dividing by zero");
    require(callback_allocations.load() == 0 && callback_releases.load() == 0, "replace and render neither allocate nor free");
}

// Control thread prepares, offers, collects, poisons and destroys plans while
// a render thread processes continuously; invalid offers are mixed in, and
// shutdown leaves an unactivated offer for the destructor.
void stress() {
    const SessionPlan initial{48000, 120000, {{1, {constant(1.0F / 64)}}}};
    auto controller = std::make_unique<RenderController>(initial);
    std::atomic<bool> done{false}, bad{false};
    std::thread render([&] {
        watching = true;
        std::array<float, 64> out{};
        while (!done.load()) {
            controller->process(out);
            for (const auto v : out) if (!std::isfinite(v) || std::abs(v) > 1) bad.store(true);
        }
        watching = false;
    });
    std::size_t tracks = 1, accepted = 0, refused = 0, retired = 0;
    std::uint64_t serial = 1;
    while (!controller->submit({ControlKind::start, {}, serial})) std::this_thread::yield();
    for (std::size_t i = 1; i <= 2000; ++i) {
        const auto next = 1 + i % 8;
        SessionPlan plan{48000, 120000, {}};
        for (std::size_t t = 0; t < next; ++t) plan.tracks.push_back({1, {constant(static_cast<float>(i % 7 + 1) / 64, 1 + (i + t) % 16)}});
        PlanMapping mapping;
        for (std::size_t t = 0; t < tracks; ++t) {
            mapping.track_from_old.push_back(t < next ? static_cast<std::int32_t>(t) : -1);
            mapping.clip_from_old.push_back({t < next ? 0 : -1});
        }
        if (i % 5 == 0) {
            auto broken = mapping;
            broken.track_from_old.push_back(0);
            if (controller->offer(replacement(plan, broken)) != OfferResult::invalid) bad.store(true);
            ++refused;
        }
        const auto at = controller->status().position + (i % 3) * 37;
        if (controller->offer(replacement(std::move(plan), std::move(mapping), at)) != OfferResult::accepted) { bad.store(true); break; }
        ++accepted;
        Retirement ack;
        while (!(ack = controller->collect()).acknowledged) std::this_thread::yield();
        if (ack.retired) {
            // The test is the plan's only owner, so overwriting it is safe
            // unless the render thread still reads it.
            if (ack.retired->plan.use_count() != 1) bad.store(true);
            for (auto& track : const_cast<SessionPlan&>(*ack.retired->plan).tracks)
                for (auto& clip : track.clips)
                    for (auto& sample : std::get<AudioClip>(clip).samples) sample = std::numeric_limits<float>::quiet_NaN();
            ack.retired.reset();
            ++retired;
        }
        tracks = next;
        CommandBatch batch;
        for (std::size_t t = 0; t < tracks; ++t) batch.commands[batch.count++] = {CommandKind::launch, 0, t, 0};
        const Control launch{ControlKind::scene, {}, ++serial, batch, LaunchPolicy::immediate};
        while (!controller->submit(launch)) std::this_thread::yield();
    }
    // Shutdown with an offer that cannot activate before the thread stops.
    SessionPlan last{48000, 120000, {{1, {constant(0.5F)}}}};
    PlanMapping mapping;
    for (std::size_t t = 0; t < tracks; ++t) { mapping.track_from_old.push_back(t ? -1 : 0); mapping.clip_from_old.push_back({t ? -1 : 0}); }
    const bool parked = controller->offer(replacement(std::move(last), std::move(mapping), std::numeric_limits<Frame>::max())) == OfferResult::accepted;
    done.store(true);
    render.join();
    const auto status = controller->status();
    controller->finish();
    controller.reset(); // destroys the active and the unactivated plan
    require(!bad.load() && parked, "no stale samples, no unexpected refusals");
    require(status.generation == accepted && retired == accepted - 1 && refused == 400 && status.rejected == 0,
        "every accepted offer activated and retired once");
    require(callback_allocations.load() == 0 && callback_releases.load() == 0, "render thread neither allocates nor frees");
}
}

int main() {
    try {
        activationBoundary();
        orderingAndRefusals();
        midiContinuity();
        stress();
        std::cout << "PASS: plan replacement at frame boundaries for blocks 1/64/257/512/3000, command remap/drop, control ordering, "
                     "refusals, MIDI voice continuity, 2000 threaded replacements with poisoned retirement and shutdown, "
                     "no render-thread allocation or release\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
