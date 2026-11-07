#include "session.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTest>
#include <QQuickWindow>
#include <QQuickItem>
#include <functional>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <string>
#include <iostream>

static ProjectSettings readAuthoredSettings(const QString& path) { const auto p = readAuthoredProject(path); return templateView(p.content, p.identities); }

// Invoked only by the owned private-server fixture. Exercises stopped graph
// replacement; optional workflow mode drives QML signals and captures software
// output in the parent fixture. This is not native input or hardware QA.
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc != 2 && argc != 3 && !(argc == 4 && (std::string_view(argv[2]) == "--record" || std::string_view(argv[2]) == "--record-loss"))) return 2;
    try {
        fl_live::PipeWireConfig config;
        config.remote = argv[1]; config.node_group = "pipewire.test";
        Session session(config);
        if (argc == 4 && std::string_view(argv[2]) == "--record-loss") {
            // Loss while recording (#46): records until the fixture kills the
            // server or destroys the input node, reports, and on "retry"
            // reconnects so takes held during the outage become clips.
            const auto track = session.trackIds()[0].toString();
            const QDir project(argv[3]), takes(project.filePath("recordings"));
            if (!session.saveProject(project.filePath("recorded.json")) || !session.setArmed(track, true)) return 1;
            const auto clips = [&] { return static_cast<int>(session.trackClips()[0].toList().size()); };
            const auto report = [&](QJsonObject extra) {
                extra["clips"] = clips(); extra["audio"] = session.audioAvailable(); extra["recording"] = session.recording();
                extra["message"] = session.message(); extra["armed"] = session.armed()[0].toBool();
                QJsonArray files; for (const auto& f : takes.entryList({"*.wav"}, QDir::Files)) files.append(takes.filePath(f));
                extra["files"] = files; extra["selected_last"] = session.selectedClips()[0] == session.trackClips()[0].toList().last().toMap()["id"];
                std::cout << QJsonDocument(extra).toJson(QJsonDocument::Compact).toStdString() << std::endl;
            };
            report({{"ready", true}});
            std::string line;
            while (std::getline(std::cin, line) && line != "record") {}
            if (!session.record()) { std::cerr << session.message().toStdString() << '\n'; return 1; }
            QElapsedTimer deadline; deadline.start();
            while (session.recording() && deadline.elapsed() < 10000) { session.tick(); QTest::qWait(5); }
            report({{"lost", !session.recording()}});
            if (session.audioAvailable()) return 0;
            while (std::getline(std::cin, line) && line != "retry") {}
            report({{"reconnected", session.reconnectAudio()}});
            while (std::getline(std::cin, line) && line != "done") {} // the fixture inspects the graph meanwhile
            return 0;
        }
        if (argc == 4) {
            // Desktop recording through the live input stream (#46/#47):
            // arming track 1 of a saved project opens fl-live-input, which
            // the fixture links; "record" records; "stop" finishes the take
            // as a clip.
            const auto track = session.trackIds()[0].toString();
            const auto project = QDir(argv[3]).filePath("recorded.json");
            if (!session.saveProject(project) || !session.setArmed(track, true)) return 1;
            const auto before = session.trackClips()[0].toList().size();
            std::cout << "{\"ready\": true}" << std::endl;
            std::string line;
            while (std::getline(std::cin, line) && line != "record") {}
            if (!session.record()) { std::cerr << session.message().toStdString() << '\n'; return 1; }
            while (std::getline(std::cin, line) && line != "stop") {}
            session.stop();
            const auto clips = session.trackClips()[0].toList();
            const auto files = QDir(QDir(argv[3]).filePath("recordings")).entryList({"*.wav"}, QDir::Files);
            const bool added = clips.size() == before + 1 && session.selectedClips()[0] == clips.last().toMap()["id"] &&
                               session.message().startsWith("Recorded 1 take") && files.size() == 1;
            const auto file = files.isEmpty() ? QString() : QDir(QDir(argv[3]).filePath("recordings")).filePath(files[0]);
            std::cout << "{\"frames\": -1"
                      << ", \"dropped\": " << (session.message().contains("dropped") ? 1 : 0)
                      << ", \"failed\": " << (added ? "false" : "true") << ", \"file\": \"" << file.toStdString()
                      << "\", \"callbacks\": -1, \"bad_buffers\": -1, \"error\": 0}" << std::endl;
            return added ? 0 : 1;
        }
        if (argc == 3 && std::string_view(argv[2]) == "--recovery") {
            const auto trackId=session.trackIds()[0],clipId=session.clipIds()[0];
            const auto sceneId=session.sceneId();
            if(!session.copyScene(sceneId) || !session.selectScene(sceneId))return 1;
            if(!session.renameTrack(trackId.toString(),"Retained audio") ||
               !session.renameClip(clipId.toString(),"Retained pulse") ||
               !session.renameScene(sceneId,"Retained scene"))return 1;
            session.setTempo(137);
            session.setPitch(0, 48);
            if(!session.setSceneIncluded(trackId.toString(),false))return 1;
            session.play();
            if(!session.launchScene(sceneId,"immediate"))return 1;
            std::cout << "{\"ready\":true}" << std::endl;
            QElapsedTimer deadline; deadline.start();
            while (session.audioAvailable() && deadline.elapsed() < 4000) QTest::qWait(10);
            if (session.audioAvailable() || session.running() || session.pending() ||
                session.activeClips()[1].toInt() != -1 || !session.dirty()) return 1;
            const auto position = session.position();
            session.play(); session.launch(1, 0); session.panic(); session.tick();
            if (session.running() || session.pending() || session.position() != position) return 1;
            QTemporaryDir directory;
            const auto path = directory.filePath("recovered.json");
            if (!session.saveProject(path) || readAuthoredSettings(path).tempo != 137 ||
                readAuthoredSettings(path).steps[0].pitch != 48 || session.dirty()) return 1;
            if(readAuthoredProject(path).identities.tracks[0].name!="Retained audio")return 1;
            if(readAuthoredProject(path).identities.scenes[0].name!="Retained scene" || session.sceneState()!="Stopped")return 1;
            if(readAuthoredProject(path).identities.scenes[0].entries.size()!=1)return 1;
            if (session.reconnectAudio() || session.audioAvailable()) return 1;
            std::cout << "{\"ready\":true}" << std::endl;
            if (std::cin.get() != 'g' || !session.reconnectAudio()) return 1;
            if (!session.audioAvailable() || session.running() || session.pending() ||
                !session.canEdit() || session.dirty() || session.position() != "1.1" ||
                session.activeClips()[1].toInt() != -1 || !session.canUndo()) return 1;
            QTest::qWait(100);
            if (session.running() || session.pending()) return 1;
            if(session.trackIds()[0]!=trackId || session.clipIds()[0]!=clipId ||
               session.trackNames()[0].toString()!="Retained audio" || session.clipNames()[0].toString()!="Retained pulse")return 1;
            if(session.sceneId()!=sceneId || session.sceneName()!="Retained scene" || session.sceneState()!="Stopped")return 1;
            if(session.sceneIds().size()!=2)return 1;
            if(session.sceneIncluded()[0].toBool() || !session.sceneIncluded()[1].toBool())return 1;
            session.undo();
            if(!session.sceneIncluded()[0].toBool() || !session.canRedo() || !session.dirty())return 1;
            session.undo();
            if(session.pitches()[0].toInt()!=60 || !session.canRedo())return 1;
            session.redo();
            if(session.pitches()[0].toInt()!=48 || !session.sceneIncluded()[0].toBool())return 1;
            session.redo();
            if (session.pitches()[0].toInt() != 48 || session.sceneIncluded()[0].toBool() || session.dirty()) return 1;
            std::cout << "PASS: server loss, failed retry, save, explicit stopped reconnect and retained history" << std::endl;
            return 0;
        }
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
            invoke("createScene","clicked");
            auto* midiInclusion=find(window->contentItem(),"sceneIncluded1");if(!midiInclusion)return 1;
            midiInclusion->setProperty("checked",true);invoke("sceneIncluded1","clicked");
            if(session.sceneIds().size()!=2 || session.selectedSceneIndex()!=1)return 1;
            invoke("copyScene","clicked");invoke("sceneUp","clicked");
            if(session.sceneIds().size()!=3 || session.selectedSceneIndex()!=1 || session.sceneIncluded()[0].toBool() || !session.sceneIncluded()[1].toBool())return 1;
            invoke("tempoSpin", "valueModified", 300);
            invoke("stepCount", "valueModified", 1);
            invoke("selectedPitch", "valueModified", 69);
            invoke("selectedVelocity", "valueModified", 50);
            invoke("selectedGate", "valueModified", 100);
            invoke("level1", "valueModified", 40);
            invoke("level0", "valueModified", 0); // Isolate MIDI in the independent capture.
            // Hard left: the right channel must stay silent. invoke() treats
            // negative values as "no value", so set the property directly.
            auto* pan=find(window->contentItem(),"pan1");if(!pan)return 1;
            pan->setProperty("value",-100);invoke("pan1","valueModified");
            auto* solo=find(window->contentItem(),"solo1");if(!solo)return 1;
            solo->setProperty("checked",true);invoke("solo1","clicked");
            if(session.pans()[1].toInt()!=-100 || !session.solos()[1].toBool())return 1;
            auto* inclusion=find(window->contentItem(),"sceneIncluded0");if(!inclusion)return 1;
            inclusion->setProperty("checked",false);invoke("sceneIncluded0","clicked");
            if(session.sceneIncluded()[0].toBool() || !session.sceneIncluded()[1].toBool())return 1;
            const auto trackId=session.trackIds()[0],clipId=session.clipIds()[1];
            invoke("renameTrack0","clicked");QTest::qWait(10);
            auto* rename=window->findChild<QObject*>("renameValue");if(!rename)return 1;
            rename->setProperty("text","Audio renamed");invoke("applyRename","clicked");QTest::qWait(10);
            invoke("renameClip1","clicked");QTest::qWait(10);
            rename->setProperty("text","A4 renamed");invoke("applyRename","clicked");QTest::qWait(10);
            if(session.trackIds()[0]!=trackId || session.clipIds()[1]!=clipId ||
               session.trackNames()[0].toString()!="Audio renamed" || session.clipNames()[1].toString()!="A4 renamed")return 1;
            if (session.tempo()!=300 || session.stepCount()!=1 || session.pitches()[0].toInt()!=69 ||
                session.velocities()[0].toInt()!=50 || session.gates()[0].toInt()!=100 ||
                session.levels()[1].toInt()!=40) return 1;
            std::cout << "{\"ready\":true}" << std::endl;
            // Fixture links and starts recording before releasing the GUI.
            if (std::cin.get() != 'g') return 1;
            invoke("playButton", "clicked");
            auto* timing=find(window->contentItem(),"sceneTiming");if(!timing)return 1;
            timing->setProperty("currentIndex",0);
            invoke("launchScene", "clicked");
            QElapsedTimer deadline; deadline.start();
            while (deadline.elapsed() < 2500) QTest::qWait(10);
            if (!session.running() || session.activeClips()[0].toInt()!=-1 || session.activeClips()[1].toInt()!=0 || session.sceneState()!="Active" || session.canEdit()) return 1;
            invoke("stopScene", "clicked");deadline.restart();
            while(session.sceneState()!="Stopped" && deadline.elapsed()<2000)QTest::qWait(10);
            if(session.sceneState()!="Stopped" || !session.running() || session.activeClips()[1].toInt()!=-1)return 1;
            QTest::qWait(350); // Capture scene-stop silence while transport keeps advancing.
            if(session.sceneState()!="Stopped" || !session.running())return 1;
            invoke("stopButton", "clicked");
            deadline.restart();
            while (!session.canEdit() && deadline.elapsed()<2000) QTest::qWait(10);
            if (!session.canEdit() || session.activeClips()[1].toInt()!=-1) return 1;
            std::cout << "PASS: QML edits, atomic scene launch, scene stop while transport runs and acknowledged transport stop" << std::endl;
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
