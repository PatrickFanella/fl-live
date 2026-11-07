// Records MIDI from a private PipeWire graph for pipewire_midi_capture.py:
//   midi-capture-probe REMOTE
// Prints {"ready":true} once fl-live-midi-input exists, records into a
// MidiTake from "record" to "stop", then prints the take as JSON.
#include "fl_live/capture.hpp"
#include "fl_live/pipewire_backend.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: midi-capture-probe REMOTE\n"; return 2; }
    try {
        fl_live::PipeWireConfig config;
        config.remote = argv[1];
        config.node_group = "pipewire.test";
        fl_live::PipeWireMidiCapture capture(config);
        fl_live::MidiTake take;
        std::cout << "{\"ready\": true}" << std::endl;
        std::string line;
        while (std::getline(std::cin, line) && line != "record") {}
        capture.setSink(&take);
        std::cout << "{\"recording\": true}" << std::endl;
        while (std::getline(std::cin, line) && line != "stop") {}
        capture.setSink(nullptr); // no data-thread use of the take after this
        const auto status = take.finish();
        const auto stream = capture.status();
        std::cout << "{\"frames\": " << status.frames << ", \"events\": " << status.events << ", \"dropped\": " << status.dropped
                  << ", \"closed_at_end\": " << status.closed_at_end << ", \"stream_events\": " << stream.events
                  << ", \"bad_buffers\": " << stream.bad_buffers << ", \"error\": " << stream.error << ", \"notes\": [";
        for (std::size_t i = 0; i < status.notes.size(); ++i) {
            const auto& n = status.notes[i];
            std::cout << (i ? ", " : "") << "[" << n.start << ", " << n.length << ", " << int(n.pitch) << ", " << int(n.velocity) << "]";
        }
        std::cout << "]}" << std::endl;
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
