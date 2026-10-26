#include "fl_live/controller.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
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
        std::cout << "PASS: 200000 ordered messages; 50000 controls with concurrent status reader; rejection, full-queue panic and restart\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
