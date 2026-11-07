#include "fl_live/pipewire_backend.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    try {
        fl_live::PipeWireConfig config;
        unsigned duration_ms = 1000;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (i + 1 >= argc) throw std::invalid_argument("each option requires a value");
            const std::string value = argv[++i];
            if (option == "--remote") config.remote = value;
            else if (option == "--target") { config.target = value; config.autoconnect = true; }
            else if (option == "--group") config.node_group = value;
            else if (option == "--channels") {
                // Parse any small count; the backend owns the supported-layout refusal.
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), config.channels);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || config.channels > 64)
                    throw std::invalid_argument("channels must be a number up to 64");
            }
            else if (option == "--duration-ms") {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), duration_ms);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || duration_ms < 100 || duration_ms > 60000)
                    throw std::invalid_argument("duration must be 100..60000 milliseconds");
            } else throw std::invalid_argument("unknown option");
        }
        if (config.remote.empty()) throw std::invalid_argument("usage: fl-live-play --remote NAME [--target NODE] [--duration-ms N] [--group NAME] [--channels 1|2]");
        // Quiet, deterministic authored fixture; no file decoding on RT thread.
        // Mono output keeps the original eight-frame loop. Stereo uses the same
        // left channel and a distinct right channel, so a swap is detectable.
        const fl_live::AudioClip mono{{0.0625F, 0.125F, 0.1875F, 0.25F, -0.0625F, -0.125F, -0.1875F, -0.25F}, true};
        const fl_live::AudioClip stereo{{0.0625F, 0.03125F, 0.125F, -0.03125F, 0.1875F, 0.09375F, 0.25F, -0.09375F,
            -0.0625F, 0.15625F, -0.125F, -0.15625F, -0.1875F, 0.21875F, -0.25F, -0.21875F}, true, 2};
        const fl_live::SessionPlan plan{48000, 120000, {{1, {config.channels == 2 ? stereo : mono}}}};
        fl_live::RenderController controller(plan);
        if (!controller.submit({fl_live::ControlKind::start, {}, 1}) ||
            !controller.submit({fl_live::ControlKind::schedule, {fl_live::CommandKind::launch, 0, 0, 0}, 2}))
            throw std::runtime_error("startup commands rejected");
        fl_live::BackendStatus backend_status{};
        fl_live::RenderStatus render_status{};
        {
            fl_live::PipeWireBackend backend(controller, config);
            std::cout << "{\"ready\":true}\n" << std::flush;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
            while (std::chrono::steady_clock::now() < deadline && backend.status().error == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            controller.panic();
            const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            while (controller.status().running && backend.status().error == 0 && std::chrono::steady_clock::now() < stop_deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            backend_status = backend.status();
        }
        render_status = controller.status();
        std::cout << "{\"callbacks\":" << backend_status.callbacks << ",\"frames\":" << backend_status.frames
            << ",\"bad_buffers\":" << backend_status.bad_buffers << ",\"max_process_ns\":" << backend_status.max_process_ns
            << ",\"processing_over_budget\":" << backend_status.processing_over_budget << ",\"error\":" << backend_status.error
            << ",\"min_render_frames\":" << backend_status.min_render_frames
            << ",\"max_render_frames\":" << backend_status.max_render_frames
            << ",\"position\":" << render_status.position << ",\"running\":" << (render_status.running ? "true" : "false")
            << ",\"handled_serial\":" << render_status.handled_serial << ",\"rejected\":" << render_status.rejected << "}\n";
        return backend_status.error || !backend_status.callbacks || render_status.running ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "fl-live-play: " << error.what() << '\n';
        return 1;
    }
}
