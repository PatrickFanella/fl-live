#include "fl_live/controller.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void track_feedback_contract() {
    using namespace fl_live;
    const SessionPlan plan{48000,120000,{
        {1,{AudioClip{{0.25F},true}, AudioClip{{0.5F,0.5F},false}}},
        {1,{MidiClip{4,{{0,2,60,1}},false}}}}};
    RenderController controller(plan);
    std::array<float,3> output{};
    auto process = [&](std::size_t n) { controller.process(std::span(output).first(n)); };
    require(controller.submit({ControlKind::start,{},1}), "feedback start"); process(1);
    require(controller.submit({ControlKind::schedule,{CommandKind::launch,4,0,0},2}), "audio queued");
    require(controller.submit({ControlKind::schedule,{CommandKind::launch,4,1,0},3}), "MIDI queued");process(1);
    auto state = controller.status();
    require(state.track_count==2 && state.tracks[0].active_clip==-1 && state.tracks[0].pending==PendingAction::launch &&
        state.tracks[0].pending_at==4 && state.tracks[0].pending_clip==0 && state.tracks[63].active_clip==-1,"queued is not active");
    process(2); require(controller.status().tracks[0].active_clip==-1,"boundary not yet rendered");
    process(1); state=controller.status();
    require(state.tracks[0].active_clip==0 && state.tracks[1].active_clip==0 &&
        state.tracks[0].pending==PendingAction::none,"rendered launch active");
    process(3); require(controller.status().tracks[1].active_clip==-1,"MIDI one-shot completion");
    require(controller.submit({ControlKind::schedule,{CommandKind::launch,8,0,1},4}),"replace with one-shot");process(1);
    require(controller.status().tracks[0].active_clip==1,"replacement clip index");
    process(1); require(controller.status().tracks[0].active_clip==-1,"audio one-shot completion");
    require(controller.submit({ControlKind::schedule,{CommandKind::launch,12,0,0},5}) &&
        controller.submit({ControlKind::schedule,{CommandKind::stop_track,12,0,0},6}),"same-frame launch/stop");process(2);
    require(controller.status().tracks[0].pending==PendingAction::launch,"earliest pending action");
    process(1); require(controller.status().tracks[0].active_clip==-1 && controller.status().tracks[0].pending==PendingAction::none,"same-frame final state");
    require(controller.submit({ControlKind::schedule,{CommandKind::launch,0,0,0},7}),"past command queued");process(1);
    require(controller.status().rejected==1 && controller.status().tracks[0].pending==PendingAction::none,"rejection must not invent pending action");
    require(controller.submit({ControlKind::launch_next_bar,{CommandKind::launch,0,0,0},8}),"quantized feedback");process(1);
    require(controller.status().tracks[0].pending_at==96000,"quantized pending time");
    controller.panic();process(1);state=controller.status();
    require(!state.running && state.tracks[0].active_clip==-1 && state.tracks[0].pending==PendingAction::none,"panic clears feedback");
}
void concurrent_track_feedback() {
    using namespace fl_live;
    const SessionPlan plan{48000,120000,{{1,{AudioClip{{0},true}}}}};
    RenderController controller(plan);
    std::atomic<bool> done{false}, bad{false};
    std::thread reader([&] {
        while (!done.load()) {
            const auto s=controller.status();
            const bool active=s.handled_serial%4==3;
            if (s.track_count!=1 || s.running!=active || s.tracks[0].active_clip!=(active ? 0 : -1) ||
                s.tracks[0].pending!=PendingAction::none) bad.store(true);
        }
    });
    std::array<float,1> output{};
    bool submitted=true;
    for (std::uint64_t n=0;n<10000;++n) {
        submitted &= controller.submit({ControlKind::rewind,{},n*4+1});
        submitted &= controller.submit({ControlKind::start,{},n*4+2});
        submitted &= controller.submit({ControlKind::launch_next_bar,{CommandKind::launch,0,0,0},n*4+3});
        controller.process(output);
        submitted &= controller.submit({ControlKind::stop,{},n*4+4}); controller.process(output);
    }
    done.store(true);reader.join();controller.finish();
    require(submitted && !bad.load(),"coherent concurrent track/transport snapshots");
}

}
int main() {
    using namespace fl_live;
    try {
        SpscQueue<std::uint64_t, 64> queue;
        std::atomic<bool> bad{false};
        std::thread producer([&] {
            for (std::uint64_t n = 1; n <= 200000; ++n)
                while (!queue.push(n)) std::this_thread::yield();
        });
        for (std::uint64_t n = 1; n <= 200000; ++n) {
            std::uint64_t received = 0;
            while (!queue.pop(received)) std::this_thread::yield();
            if (received != n) bad.store(true);
        }
        producer.join();
        require(!bad.load(), "concurrent SPSC values lost/reordered");
        const SessionPlan plan{48000, 120000, {{1, {AudioClip{{0.25F}, true}}}}};
        RenderController concurrent(plan);
        std::atomic<bool> finished{false};
        std::thread control([&] {
            for (std::uint64_t n = 1; n <= 50000; ++n)
                while (!concurrent.submit({n % 2 ? ControlKind::start : ControlKind::stop, {}, n}))
                    std::this_thread::yield();
        });
        std::thread reader([&] {
            Frame previous = 0;
            while (!finished.load()) {
                const auto snapshot = concurrent.status();
                if (snapshot.position < previous || snapshot.running != static_cast<bool>(snapshot.handled_serial % 2) || snapshot.rejected)
                    bad.store(true);
                previous = snapshot.position;
            }
        });
        std::array<float, 64> concurrent_output{};
        while (concurrent.status().handled_serial < 50000) concurrent.process(concurrent_output);
        control.join(); finished.store(true); reader.join(); concurrent.finish();
        require(!bad.load() && !concurrent.status().running, "concurrent snapshot coherence");
        RenderController controller(plan);
        require(controller.submit({ControlKind::start, {}, 1}), "start submit");
        require(controller.submit({ControlKind::schedule, {CommandKind::launch, 0, 0, 0}, 2}), "launch submit");
        std::array<float, 64> output{};
        controller.process(output);
        auto status = controller.status();
        require(status.running && status.position == 64 && status.handled_serial == 2 && output[0] == 0.25F,
                "render acknowledgement");
        require(controller.submit({ControlKind::schedule, {CommandKind::launch, 0, 0, 0}, 3}), "past submit");
        controller.process(output);
        require(controller.status().rejected == 1 && controller.status().handled_serial == 3, "rejection acknowledgement");
        for (std::uint64_t n = 4; n < 68; ++n) require(controller.submit({ControlKind::start, {}, n}), "queue capacity");
        require(!controller.submit({ControlKind::stop, {}, 68}), "queue must reject overflow");
        controller.panic();
        controller.process(output);
        status = controller.status();
        require(!status.running && status.position == 128 && status.handled_serial == 67, "panic priority/drain");
        for (auto sample : output) require(sample == 0, "panic not silent");
        require(controller.submit({ControlKind::rewind, {}, 69}) && controller.submit({ControlKind::start, {}, 70}), "restart submit");
        controller.process(output);
        require(controller.status().position == 64 && controller.status().running, "explicit restart after panic");
        controller.panic(); controller.process(output); controller.finish();
        track_feedback_contract(); concurrent_track_feedback();
        std::cout << "PASS: track timing, one-shots and 10000 concurrent launch/stop cycles; 200000 ordered messages; 50000 controls with concurrent status reader; rejection, full-queue panic and restart\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
