// Records from a private PipeWire graph for pipewire_capture.py:
//   capture-probe REMOTE OUTPUT.wav CHANNELS
// Prints {"ready":true} once its input node exists, starts recording on a
// "record" line, stops on "stop" and prints the take's status as JSON.
#include "fl_live/capture.hpp"
#include "fl_live/pipewire_backend.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) { std::cerr << "usage: capture-probe REMOTE OUTPUT.wav CHANNELS\n"; return 2; }
    try {
        fl_live::PipeWireConfig config;
        config.remote = argv[1];
        config.channels = static_cast<std::uint32_t>(std::stoul(argv[3]));
        config.node_group = "pipewire.test";
        fl_live::PipeWireCapture capture(config);
        fl_live::CaptureTake take(argv[2], config.channels, 48000);
        std::cout << "{\"ready\": true}" << std::endl;
        std::string line;
        while (std::getline(std::cin, line) && line != "record") {}
        capture.setSink(&take);
        while (std::getline(std::cin, line) && line != "stop") {}
        capture.setSink(nullptr); // no data-thread use of the take after this
        const auto status = take.finish();
        const auto stream = capture.status();
        std::cout << "{\"frames\": " << status.frames_written << ", \"dropped\": " << status.frames_dropped
                  << ", \"failed\": " << (status.failed ? "true" : "false") << ", \"file\": \"" << status.file.string()
                  << "\", \"callbacks\": " << stream.callbacks << ", \"bad_buffers\": " << stream.bad_buffers
                  << ", \"error\": " << stream.error << "}" << std::endl;
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
