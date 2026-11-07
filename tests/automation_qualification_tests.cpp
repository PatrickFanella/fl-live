// Automation output qualification (#57): an independent oracle checks gain
// and pan lanes together with render-owned bar boundaries at a fractional
// tempo, across non-dividing block sizes, including a lane changed by a plan
// replacement during playback. A threaded run replaces automation
// continuously and checks that every rendered block comes from one plan.
#include "fl_live/controller.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local bool watching = false;
std::atomic<std::size_t> callback_allocations{0};
}
void* operator new(std::size_t size) {
    if (watching) callback_allocations.fetch_add(1);
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

constexpr std::uint32_t rate = 48000, bpm_milli = 133333; // 133.333 BPM: bars are 86400.22 frames
// Independent boundary: first frame at or after k bars, from 128-bit integers.
Frame bar(unsigned k) {
    const unsigned __int128 numerator = static_cast<unsigned __int128>(k) * rate * 240000U;
    return static_cast<Frame>((numerator + bpm_milli - 1) / bpm_milli);
}
// The declared policy, stated independently of Engine::automationValue.
float lane(const std::vector<AutomationPoint>& points, Frame f) {
    if (f <= points.front().at) return points.front().value;
    if (f >= points.back().at) return points.back().value;
    std::size_t i = 0;
    while (points[i + 1].at <= f) ++i;
    const double t = static_cast<double>(f - points[i].at) / static_cast<double>(points[i + 1].at - points[i].at);
    return static_cast<float>(points[i].value + (static_cast<double>(points[i + 1].value) - points[i].value) * t);
}
struct Lanes { std::vector<AutomationPoint> gain, pan; };
SessionPlan session(const Lanes& lanes) {
    SessionPlan plan{rate, bpm_milli, {}};
    plan.tracks.push_back({1, {AudioClip{{0.5F, 0.25F, -0.5F}, true}}, 0, lanes.gain, {}});
    plan.tracks.push_back({1, {AudioClip{{-0.25F}, true}}, 0, {}, lanes.pan});
    return plan;
}

// Track 0 and 1 launch on the next bar from a mid-bar position; track 1 stops
// on the bar after a later mid-bar request; at `swap` a replacement changes
// track 0's gain lane. All requests are made between blocks, so their
// position varies with the block size but their bar does not.
const Lanes first{{{bar(1), 0.0F}, {bar(1) + 30001, 1.0F}, {bar(2) + 777, 0.25F}}, {{0, -1.0F}, {bar(3), 1.0F}}};
const Lanes second{{{bar(1), 1.0F}, {bar(2) + 50000, 0.0F}, {bar(3) - 1, 0.6F}}, first.pan};
const Frame swap = bar(1) + 50000, frames = bar(3) + 10000;
std::vector<float> render(std::size_t block) {
    const auto plan = session(first);
    RenderController controller(plan);
    std::vector<float> out(frames * 2);
    bool launched = false, offered = false, stopped = false;
    std::uint64_t serial = 0;
    require(controller.submit({ControlKind::start, {}, ++serial}), "start");
    for (Frame at = 0; at < frames;) {
        const auto position = controller.status().position;
        if (!launched && position >= 1000) {
            CommandBatch both; both.commands[0] = {CommandKind::launch, 0, 0, 0}; both.commands[1] = {CommandKind::launch, 0, 1, 0}; both.count = 2;
            require(controller.submit({ControlKind::scene, {}, ++serial, both, LaunchPolicy::next_bar}), "launch");
            launched = true;
        }
        if (!offered && position >= 20000) {
            auto next = std::make_unique<PlanReplacement>(PlanReplacement{std::make_shared<const SessionPlan>(session(second)), {{0, 1}, {{0}, {0}}}, swap});
            require(controller.offer(std::move(next)) == OfferResult::accepted, "offer");
            offered = true;
        }
        if (!stopped && position >= bar(2) + 100) {
            require(controller.submit({ControlKind::stop_next_bar, {CommandKind::stop_track, 0, 1, 0}, ++serial}), "stop");
            stopped = true;
        }
        const auto count = std::min<Frame>(block, frames - at);
        watching = true;
        controller.process(std::span(out).subspan(at * 2, count * 2), 2);
        watching = false;
        at += count;
    }
    require(controller.status().generation == 1 && controller.status().rejected == 0, "one activation, no rejections");
    return out;
}
void fractionalTempoOracle() {
    require(bar(1) == 86401 && bar(2) == 172801 && bar(3) == 259201, "independent bar boundaries at 133.333 BPM");
    const TempoGrid grid(rate, bpm_milli);
    for (unsigned k = 1; k <= 3; ++k) require(grid.next_bar(bar(k - 1) + 1) == bar(k), "engine grid agrees with the oracle");
    const auto reference = render(64);
    std::size_t checked = 0;
    for (Frame f = 0; f < frames; ++f) {
        float left = 0, right = 0;
        if (f >= bar(1)) {
            const float gain = lane(f < swap ? first.gain : second.gain, f);
            const float source = std::array{0.5F, 0.25F, -0.5F}[(f - bar(1)) % 3];
            left += gain * source; right += gain * source;
            if (f < bar(3)) {
                const float pan = lane(first.pan, f);
                left += (pan > 0 ? 1 - pan : 1.0F) * -0.25F;
                right += (pan < 0 ? 1 + pan : 1.0F) * -0.25F;
            }
        }
        if (std::abs(reference[2 * f] - left) > 1e-6F || std::abs(reference[2 * f + 1] - right) > 1e-6F)
            throw std::runtime_error("output differs from the oracle at frame " + std::to_string(f));
        ++checked;
    }
    require(checked == frames, "every frame checked");
    for (const auto block : {1U, 127U, 257U, 1000U, 4096U})
        require(render(block) == reference, "identical output for every block size");
    require(callback_allocations.load() == 0, "no allocation while processing");
}

// The render thread processes while the control thread replaces automation
// lanes as fast as they activate. Every lane in plan g is the constant g/1024,
// so each output frame must equal source * g for one plan g at a time, and g
// must never decrease.
void concurrentAutomationReplacement() {
    const auto constant = [](std::uint32_t g) {
        SessionPlan plan{rate, bpm_milli, {}};
        const float value = static_cast<float>(g) / 1024;
        plan.tracks.push_back({1, {AudioClip{{1.0F}, true}}, 0, {{0, value}, {1000000, value}}, {}});
        return plan;
    };
    const auto initial = constant(1);
    RenderController controller(initial);
    std::atomic<bool> done{false}, bad{false};
    std::thread render([&] {
        std::array<float, 64 * 2> out{};
        float last = 0;
        while (!done.load()) {
            controller.process(out, 2);
            for (const auto v : out) {
                if (v != 0 && (v < last || std::fmod(v * 1024, 1.0F) != 0)) bad.store(true);
                if (v != 0) last = v;
            }
        }
    });
    require(controller.submit({ControlKind::start, {}, 1}) && controller.submit({ControlKind::schedule, {CommandKind::launch, 0, 0, 0}, 2}), "start");
    std::uint32_t accepted = 0;
    for (std::uint32_t g = 2; g <= 1024; ++g) {
        while (controller.collect().acknowledged) {}
        auto next = std::make_unique<PlanReplacement>(PlanReplacement{std::make_shared<const SessionPlan>(constant(g)), {{0}, {{0}}}, 0});
        while (controller.offer(std::move(next)) != OfferResult::accepted) {
            while (controller.collect().acknowledged) {}
            std::this_thread::yield();
            next = std::make_unique<PlanReplacement>(PlanReplacement{std::make_shared<const SessionPlan>(constant(g)), {{0}, {{0}}}, 0});
        }
        ++accepted;
    }
    while (controller.status().generation < accepted) std::this_thread::yield();
    done.store(true);
    render.join();
    require(!bad.load() && controller.status().generation == 1023, "each block follows one plan and lanes never step back");
}
}

int main() {
    try {
        fractionalTempoOracle();
        concurrentAutomationReplacement();
        std::cout << "PASS: gain/pan lanes with next-bar launch/stop at 133.333 BPM match an independent oracle on every frame, "
                     "including a lane replaced mid-playback; identical at blocks 1/64/127/257/1000/4096; 1023 concurrent "
                     "automation replacements stay monotonic; no allocation while processing\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
