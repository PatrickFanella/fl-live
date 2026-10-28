#include "session.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTest>
#include <QQuickWindow>
#include <QQuickItem>
#include <functional>
#include <QElapsedTimer>
#include <iostream>

// Invoked only by the owned private-server fixture. Exercises stopped graph
// replacement; optional workflow mode drives QML signals and captures software
// output in the parent fixture. This is not native input or hardware QA.
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc != 2 && argc != 3) return 2;
    try {
        fl_live::PipeWireConfig config;
        config.remote = argv[1]; config.node_group = "pipewire.test";
        Session session(config);
        if (argc == 3) {
            QQmlApplicationEngine engine;
            engine.rootContext()->setContextProperty("session", &session);
            engine.load(QUrl("qrc:/ui/Main.qml"));
            if (engine.rootObjects().isEmpty()) return 1;
            auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (!window || !QTest::qWaitForWindowExposed(window)) return 1;
            std::function<QQuickItem*(QQuickItem*, const QString&)> find =
                [&](QQuickItem* item, const QString& name) -> QQuickItem* {
                    if (item->objectName()==name) return item;
                    for (auto* child : item->childItems()) if (auto* result=find(child,name)) return result;
                    return nullptr;
                };
            auto invoke = [&](const char* name, const char* signal, int value = -1) {
                auto* control = find(window->contentItem(), QString::fromUtf8(name));
                if (!control || !control->property("enabled").toBool()) throw std::runtime_error(name);
                if (value >= 0) control->setProperty("value", value);
                if (!QMetaObject::invokeMethod(control, signal)) throw std::runtime_error(signal);
            };
            QTest::qWait(50);
            invoke("tempoSpin", "valueModified", 300);
            invoke("stepCount", "valueModified", 1);
            invoke("selectedPitch", "valueModified", 69);
            invoke("selectedVelocity", "valueModified", 50);
            invoke("selectedGate", "valueModified", 100);
            invoke("level1", "valueModified", 40);
            if (session.tempo()!=300 || session.stepCount()!=1 || session.pitches()[0].toInt()!=69 ||
                session.velocities()[0].toInt()!=50 || session.gates()[0].toInt()!=100 ||
                session.levels()[1].toInt()!=40) return 1;
            std::cout << "{\"ready\":true}" << std::endl;
            // Fixture links and starts recording before releasing the GUI.
            if (std::cin.get() != 'g') return 1;
            invoke("playButton", "clicked");
            invoke("clip1", "clicked");
            QElapsedTimer deadline; deadline.start();
            while (deadline.elapsed() < 2500) QTest::qWait(10);
            if (!session.running() || session.activeClips()[1].toInt()!=0 || session.canEdit()) return 1;
            invoke("stopButton", "clicked");
            deadline.restart();
            while (!session.canEdit() && deadline.elapsed()<2000) QTest::qWait(10);
            if (!session.canEdit() || session.activeClips()[1].toInt()!=-1) return 1;
            std::cout << "PASS: QML edits, play, MIDI launch and acknowledged stop" << std::endl;
            return 0;
        }
        session.setTempo(137);
        session.setLevel(0,35);
        session.setPitch(0,48);
        if (session.tempo()!=137 || session.levels()[0].toInt()!=35 || session.pitches()[0].toInt()!=48 ||
            !session.canEdit() || !session.dirty()) {
            std::cerr << session.message().toStdString() << '\n';
            return 1;
        }
        session.undo();
        if (session.pitches()[0].toInt()!=60 || !session.canRedo()) return 1;
        session.redo();
        if (session.pitches()[0].toInt()!=48 || !session.canUndo()) return 1;
        std::cout << "PASS: five stopped project rebuilds including Undo/Redo on the private PipeWire server\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
