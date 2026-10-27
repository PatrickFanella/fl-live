#include "session.hpp"
#include <QCoreApplication>
#include <iostream>

// Invoked only by the owned private-server fixture. Exercises stopped graph
// replacement and backend teardown/recreation, not audible output or UI input.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    try {
        fl_live::PipeWireConfig config;
        config.remote = argv[1]; config.node_group = "pipewire.test";
        Session session(config);
        session.setTempo(137);
        session.setLevel(0,35);
        session.setPitch(0,48);
        if (session.tempo()!=137 || session.levels()[0].toInt()!=35 || session.pitches()[0].toInt()!=48 ||
            !session.canEdit() || !session.dirty()) {
            std::cerr << session.message().toStdString() << '\n';
            return 1;
        }
        std::cout << "PASS: three stopped project rebuilds on the private PipeWire server\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
