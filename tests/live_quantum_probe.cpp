#include "fl_live/pipewire_backend.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Owned private-server test client. Its plan stays immutable for the entire
// run; metadata changes belong to the parent fixture, never this callback.
int main(int argc, char** argv) {
    if (argc != 2 && !(argc == 3 && std::string_view(argv[2]) == "--fixed")) return 2;
    try {
        // Long, frame-coded waveform avoids hiding a lost/duplicated callback
        // behind the headless demo's eight-frame repetition. Binary fractions
        // are exactly representable in both C++ float and Python's oracle.
        std::vector<float> samples(65521);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<float>(static_cast<int>(i) - 32760) / 131072.0F;
        const fl_live::SessionPlan plan{48000, 120000,
            {{1, {fl_live::AudioClip{std::move(samples), true}}}}};
        fl_live::RenderController controller(plan);
        if (!controller.submit({fl_live::ControlKind::start, {}, 1}) ||
            !controller.submit({fl_live::ControlKind::schedule, {fl_live::CommandKind::launch, 0, 0, 0}, 2}))
            throw std::runtime_error("startup commands rejected");
        fl_live::BackendStatus final{};
        {
            fl_live::PipeWireConfig config;
            config.remote = argv[1]; config.node_group = "pipewire.test";
            fl_live::PipeWireBackend backend(controller, config);
            std::cout << "{\"ready\":true}" << std::endl;
            for (const auto quantum : {257U, 1024U, 128U}) {
                if (argc == 3 && quantum != 257) break;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                for (;;) {
                    const auto status = backend.status();
                    if (status.error || status.bad_buffers) throw std::runtime_error("backend failed during transition");
                    const bool observed = quantum == 257 ? status.min_render_frames == 257 && status.max_render_frames == 257 :
                        quantum == 1024 ? status.min_render_frames == 257 && status.max_render_frames == 1024 :
                        status.min_render_frames == 128 && status.max_render_frames == 1024;
                    if (observed && controller.status().running) {
                        std::cout << "{\"ready\":true,\"quantum\":" << quantum
                            << ",\"frames\":" << status.frames << ",\"callbacks\":" << status.callbacks << "}" << std::endl;
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("quantum observation timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            // Backend processing continues while the parent finishes capture.
            if (std::cin.get() != 'g') return 1;
            final = backend.status();
        }
        const auto stopped = controller.status();
        if (final.error || final.bad_buffers || stopped.running || stopped.handled_serial != 2 || stopped.rejected)
            throw std::runtime_error("invalid final backend/transport state");
        std::cout << "{\"passed\":true,\"frames\":" << final.frames
            << ",\"callbacks\":" << final.callbacks << ",\"min_render_frames\":" << final.min_render_frames
            << ",\"max_render_frames\":" << final.max_render_frames << ",\"running\":false}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
