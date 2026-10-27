#include "session.hpp"
#include <QtTest>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <array>
#include <functional>
#include <limits>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

class DesktopTests : public QObject {
    Q_OBJECT
private slots:
    void legacyMigration() {
        QTemporaryDir directory;
        const auto path=directory.filePath("legacy.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"format":"fl-live-project","version":1,"template":"pulse-steps-v1","tempo":120,"levels":[70,25],"pitches":[60,64,67,72]})");file.close();
        const auto settings=readProject(path);
        QCOMPARE(settings.step_count,4);
        auto plan=prepareProject(settings);
        const auto& midi=std::get<fl_live::MidiClip>(plan.tracks[1].clips[0]);
        QCOMPARE(midi.length,fl_live::Frame(96000));QCOMPARE(midi.notes.size(),std::size_t(4));
        for (std::size_t i=0;i<4;++i) {
            QCOMPARE(midi.notes[i].start,fl_live::Frame(i*24000));
            QCOMPARE(midi.notes[i].duration,fl_live::Frame(18000));
            QCOMPARE(midi.notes[i].velocity,0.7F);
        }
        QVERIFY(!settings.steps[4].enabled);
        const auto converted=directory.filePath("converted.json");writeProject(converted,settings);
        QVERIFY(readProject(converted)==settings);
        QFile convertedFile(converted);QVERIFY(convertedFile.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(convertedFile.readAll()).object()["version"].toInt(),2);
    }
    void patternTimingAndRests() {
        ProjectSettings settings;
        settings.step_count=16;settings.levels={0,100};
        for (auto& step:settings.steps) step={};
        settings.steps[0]={true,60,100,25};settings.steps[2]={true,64,50,100};settings.steps[15]={true,127,0,1};
        auto plan=prepareProject(settings);
        const auto& midi=std::get<fl_live::MidiClip>(plan.tracks[1].clips[0]);
        QCOMPARE(midi.length,fl_live::Frame(384000));QCOMPARE(midi.notes.size(),std::size_t(3));
        QCOMPARE(midi.notes[0].duration,fl_live::Frame(6000));
        QCOMPARE(midi.notes[1].start,fl_live::Frame(48000));QCOMPARE(midi.notes[1].duration,fl_live::Frame(24000));
        QCOMPARE(midi.notes[1].velocity,0.5F);
        QCOMPARE(midi.notes[2].start,fl_live::Frame(360000));QCOMPARE(midi.notes[2].duration,fl_live::Frame(240));
        fl_live::Engine engine(plan);engine.start();
        QCOMPARE(engine.schedule({fl_live::CommandKind::launch,0,1,0}),fl_live::ScheduleResult::accepted);
        std::vector<float> output(48000);engine.process(output);
        QVERIFY(std::any_of(output.begin(),output.begin()+6000,[](float x){return x!=0;}));
        QVERIFY(std::all_of(output.begin()+6000,output.end(),[](float x){return x==0;}));
        settings.steps[0].velocity=50;const auto quietPlan=prepareProject(settings);fl_live::Engine quiet(quietPlan);quiet.start();
        QCOMPARE(quiet.schedule({fl_live::CommandKind::launch,0,1,0}),fl_live::ScheduleResult::accepted);
        std::vector<float> quietOutput(48000);quiet.process(quietOutput);
        for (std::size_t i=0;i<6000;++i) QVERIFY(std::abs(output[i]-quietOutput[i]*2)<0.000001F);
        for (auto& step:settings.steps) step.enabled=false;
        const auto rests=prepareProject(settings);QVERIFY(std::get<fl_live::MidiClip>(rests.tracks[1].clips[0]).notes.empty());
        fl_live::Engine silent(rests);silent.start();
        QCOMPARE(silent.schedule({fl_live::CommandKind::launch,0,1,0}),fl_live::ScheduleResult::accepted);
        silent.process(output);QVERIFY(std::all_of(output.begin(),output.end(),[](float x){return x==0;}));
        for (int tempo:{20,137,300}) for (int length=1;length<=16;++length) {
            settings.tempo=tempo;settings.step_count=length;
            for (auto& step:settings.steps) step={true,60,70,100};
            const auto prepared=prepareProject(settings);fl_live::Engine validated(prepared);
            const auto& notes=std::get<fl_live::MidiClip>(prepared.tracks[1].clips[0]);
            QCOMPARE(notes.length,(48000ULL*60*static_cast<unsigned>(length)+static_cast<unsigned>(tempo)-1)/static_cast<unsigned>(tempo));
            QCOMPARE(notes.notes.back().start+notes.notes.back().duration,notes.length);
        }
    }
    void stepEditingAndValidation() {
        Session session;session.selectStep(1);QVERIFY(!session.dirty());
        session.setStepCount(16);session.selectStep(15);QCOMPARE(session.selectedStep(),15);
        session.setStepEnabled(15,true);session.setPitch(15,40);session.setVelocity(15,35);session.setGate(15,100);
        session.setStepCount(4);QCOMPARE(session.selectedStep(),3);QCOMPARE(session.pitches()[15].toInt(),40);
        session.setStepCount(16);QVERIFY(session.stepEnabled()[15].toBool());QCOMPARE(session.velocities()[15].toInt(),35);
        session.play();session.setGate(15,1);QCOMPARE(session.gates()[15].toInt(),100);
        session.tick();session.setStepCount(1);QCOMPARE(session.stepCount(),16);session.stop();session.tick();
        QTemporaryDir directory;const auto good=directory.filePath("good.json");QVERIFY(session.saveProject(good));
        QFile file(good);QVERIFY(file.open(QIODevice::ReadOnly));const auto document=QJsonDocument::fromJson(file.readAll()).object();file.close();
        const auto bad=directory.filePath("bad.json");
        auto reject=[&](QJsonObject object) {
            QFile badFile(bad);QVERIFY(badFile.open(QIODevice::WriteOnly|QIODevice::Truncate));badFile.write(QJsonDocument(object).toJson());badFile.close();
            QVERIFY(!session.loadProject(bad));QCOMPARE(session.stepCount(),16);QCOMPARE(session.pitches()[15].toInt(),40);QVERIFY(!session.dirty());
        };
        for (int length:{0,17}) {auto object=document;auto pattern=object["pattern"].toObject();pattern["length"]=length;object["pattern"]=pattern;reject(object);}
        for (const auto& key:{QString("pitch"),QString("velocity"),QString("gate"),QString("enabled")}) {
            auto object=document;auto pattern=object["pattern"].toObject();auto steps=pattern["steps"].toArray();auto step=steps[0].toObject();
            step[key]=-1;steps[0]=step;pattern["steps"]=steps;object["pattern"]=pattern;reject(object);
        }
        auto object=document;auto pattern=object["pattern"].toObject();auto steps=pattern["steps"].toArray();steps.removeLast();pattern["steps"]=steps;object["pattern"]=pattern;reject(object);
        session.setVelocity(0,101);QCOMPARE(session.velocities()[0].toInt(),70);
        session.setGate(0,0);QCOMPARE(session.gates()[0].toInt(),75);
        QVERIFY(session.loadProject(good));QCOMPARE(session.selectedStep(),0);QCOMPARE(session.gates()[15].toInt(),100);
    }
    void projectRoundTrip() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("session.json");
        ProjectSettings settings;
        settings.tempo = 137; settings.levels = {35, 60};
        settings.step_count=12;settings.steps[9]={true,72,32,100};settings.steps[1].enabled=false; const std::array<int,4> pitches{48,55,59,62};
        for (std::size_t i=0;i<4;++i) settings.steps[i].pitch=pitches[i];
        writeProject(path, settings);
        const auto decoded = readProject(path);
        QVERIFY(decoded == settings);
        auto render = [](const ProjectSettings& value) {
            const auto plan = prepareProject(value);
            fl_live::Engine engine(plan);
            engine.start();
            if (engine.schedule({fl_live::CommandKind::launch, 0, 0, 0}) != fl_live::ScheduleResult::accepted ||
                engine.schedule({fl_live::CommandKind::launch, 0, 1, 0}) != fl_live::ScheduleResult::accepted)
                throw std::runtime_error("test launch failed");
            std::vector<float> samples(180000);
            engine.process(samples);
            return samples;
        };
        const auto before = render(settings);
        QVERIFY(before == render(decoded));
        QVERIFY(before != render(ProjectSettings{}));
        for (int tempo : {20, 137, 300}) {
            settings.tempo = tempo;
            const auto plan = prepareProject(settings);
            fl_live::Engine validated(plan);
            QCOMPARE(std::get<fl_live::MidiClip>(plan.tracks[1].clips[0]).length,
                (48000ULL*60*static_cast<unsigned>(settings.step_count)+static_cast<unsigned>(tempo)-1)/static_cast<unsigned>(tempo));
        }
    }
    void documentSafety() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session session;
        session.setTempo(137); session.setLevel(0,35); session.setPitch(0,48);
        QVERIFY(session.dirty());
        const auto path = directory.filePath("saved.json");
        QVERIFY(session.saveProject(path)); QVERIFY(!session.dirty());
        session.setTempo(150); QVERIFY(session.dirty());
        QVERIFY(!session.loadProject(path)); QCOMPARE(session.tempo(),150);
        const auto other = directory.filePath("other.json");
        QVERIFY(session.saveProject(other));
        QVERIFY(session.loadProject(path)); QCOMPARE(session.tempo(),137);
        QCOMPARE(session.levels()[0].toInt(),35); QCOMPARE(session.pitches()[0].toInt(),48);
        QCOMPARE(session.position(),QString("1.1"));
        session.play();
        session.setTempo(180); QCOMPARE(session.tempo(),137); // queued start blocks editing
        session.tick();
        session.setTempo(200); QCOMPARE(session.tempo(),137);
        QVERIFY(!session.loadProject(path)); QVERIFY(session.running());
        session.stop(); session.tick();
        QFile file(directory.filePath("unrelated.json"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("keep me"); file.close();
        QVERIFY(!session.saveProject(file.fileName()));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("keep me")); file.close();
        QVERIFY(!session.loadProject(file.fileName())); QCOMPARE(session.tempo(),137);
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QByteArray(65537,' '));file.close();
        QVERIFY(!session.loadProject(file.fileName())); QCOMPARE(session.tempo(),137);
        QFile valid(path); QVERIFY(valid.open(QIODevice::ReadOnly));
        const auto document = QJsonDocument::fromJson(valid.readAll()).object(); valid.close();
        for (const auto& key : {QString("version"),QString("tempo"),QString("levels"),QString("pattern"),QString("template")}) {
            auto broken = document; broken.insert(key, -1);
            QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate)); file.write(QJsonDocument(broken).toJson());file.close();
            QVERIFY(!session.loadProject(file.fileName())); QCOMPARE(session.tempo(),137);
        }
        session.setLevel(0,101); QCOMPARE(session.levels()[0].toInt(),35); QVERIFY(!session.dirty());
        session.setPitch(0,-1); QCOMPARE(session.pitches()[0].toInt(),48);
        QVERIFY(!session.saveProject(directory.filePath("missing/child.json"))); QVERIFY(!session.dirty());
        session.setTempo(160); QVERIFY(session.saveProject(path)); QCOMPARE(readProject(path).tempo,160);
    }
    void clipFeedback() {
        Session session;
        QCOMPARE(session.activeClips()[0].toInt(),-1);
        QCOMPARE(session.clipStates()[0].toString(),QString("Stopped"));
        session.launch(0,0); QCOMPARE(session.clipStates()[0].toString(),QString("Launch requested"));
        session.tick(); QCOMPARE(session.clipStates()[0].toString(),QString("Launch queued"));
        QCOMPARE(session.activeClips()[0].toInt(),-1);
        session.play();session.tick();
        QCOMPARE(session.activeClips()[0].toInt(),0); QCOMPARE(session.clipStates()[0].toString(),QString("Active"));
        session.stopTrack(0); QCOMPARE(session.clipStates()[0].toString(),QString("Stop requested"));
        session.tick(); QCOMPARE(session.clipStates()[0].toString(),QString("Active · stop queued"));
        for (int n=0;n<198;++n) session.tick();
        QCOMPARE(session.activeClips()[0].toInt(),0); // frame 96000 has not yet been rendered
        session.tick(); QCOMPARE(session.activeClips()[0].toInt(),-1);
        QCOMPARE(session.clipStates()[0].toString(),QString("Stopped"));
        session.launch(0,0);session.tick();QCOMPARE(session.clipStates()[0].toString(),QString("Launch queued"));
        session.panic();session.tick();QCOMPARE(session.clipStates()[0].toString(),QString("Stopped"));
        session.setTempo(137);QCOMPARE(session.activeClips()[0].toInt(),-1);
    }
    void acknowledgement() {
        Session session;
        session.play();
        QVERIFY(!session.running());
        QVERIFY(session.pending());
        session.tick();
        QVERIFY(session.running());
        QVERIFY(!session.pending());
        session.stop(); session.tick(); QVERIFY(!session.running());
        session.rewind(); session.tick(); QCOMPARE(session.position(), QString("1.1"));
        session.launch(9, 0); QCOMPARE(session.message(), QString("Unknown clip"));
        session.play(); session.tick(); session.panic(); session.tick(); QVERIFY(!session.running());
    }
    void renderOwnedBoundary() {
        using namespace fl_live;
        const SessionPlan plan{48000,120000,{{1,{AudioClip{{0.25F},true}}}}};
        const TempoGrid grid(48000,120000);
        QCOMPARE(grid.try_next_bar(96000), std::optional<Frame>(96000));
        QVERIFY(!grid.try_next_bar(std::numeric_limits<Frame>::max()));
        RenderController controller(plan);
        std::array<float,257> block{};
        QVERIFY(controller.submit({ControlKind::start,{},1}));
        controller.process(block); // request arrives after the first bar boundary
        QVERIFY(controller.submit({ControlKind::launch_next_bar,{CommandKind::launch,0,0,0},2}));
        while (controller.status().position < 96200) {
            const auto begin = controller.status().position;
            controller.process(block);
            for (std::size_t i=0; i<block.size(); ++i)
                QCOMPARE(block[i], begin+i < 96000 ? 0.0F : 0.25F);
        }
        QCOMPARE(controller.status().rejected, std::uint64_t(0));
        QVERIFY(controller.submit({ControlKind::stop_next_bar,{CommandKind::stop_track,0,0,0},3}));
        while (controller.status().position < 192200) {
            const auto begin = controller.status().position;
            controller.process(block);
            for (std::size_t i=0; i<block.size(); ++i)
                QCOMPARE(block[i], begin+i < 192000 ? 0.25F : 0.0F);
        }
    }
    void qmlControls() {
        Session session;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("session", &session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        QCOMPARE(engine.rootObjects().size(), 1);
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*, const QString&)> findItem =
            [&](QQuickItem* root, const QString& name) -> QQuickItem* {
                if (root->objectName() == name && root->isVisible()) return root;
                for (auto* child : root->childItems())
                    if (auto* match = findItem(child, name)) return match;
                return nullptr;
            };
        auto click = [&](const char* name) {
            auto* item = findItem(window->contentItem(), QString::fromUtf8(name));
            QVERIFY(item);
            const auto point = item->mapToScene(QPointF(item->width()/2,item->height()/2));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point.toPoint());
        };
        auto* tempo = findItem(window->contentItem(), "tempoSpin");
        QVERIFY(tempo); tempo->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Up);
        QCOMPARE(session.tempo(),121); QVERIFY(session.dirty());
        auto key=[&](const char* name,Qt::Key key) {
            auto* item=findItem(window->contentItem(),QString::fromUtf8(name));QVERIFY(item);item->forceActiveFocus();QTest::keyClick(window,key);
        };
        key("stepCount",Qt::Key_Up);QCOMPARE(session.stepCount(),5);
        click("step4");QCOMPARE(session.selectedStep(),4);
        click("stepEnabled");QVERIFY(session.stepEnabled()[4].toBool());
        key("selectedPitch",Qt::Key_Up);QCOMPARE(session.pitches()[4].toInt(),61);
        key("selectedVelocity",Qt::Key_Down);QCOMPARE(session.velocities()[4].toInt(),69);
        key("selectedGate",Qt::Key_Down);QCOMPARE(session.gates()[4].toInt(),74);
        QTemporaryDir directory;
        auto* projectFile = window->findChild<QObject*>("projectFile");
        QVERIFY(projectFile); projectFile->setProperty("text",directory.filePath("qml.json"));
        QVERIFY(!window->close()); // dirty close must leave the window open
        auto* dialog = window->findChild<QObject*>("closeDialog");
        QVERIFY(dialog); QVERIFY(dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog,"close"));
        click("saveProjectButton"); QVERIFY(!session.dirty());
        const auto saved=readProject(directory.filePath("qml.json"));QCOMPARE(saved.tempo,121);
        QCOMPARE(saved.step_count,5);QCOMPARE(saved.steps[4].pitch,61);QCOMPARE(saved.steps[4].velocity,69);QCOMPARE(saved.steps[4].gate,74);
        click("openProjectButton"); QCOMPARE(session.tempo(),121);
        click("step4");QCOMPARE(session.selectedStep(),4);
        QCOMPARE(findItem(window->contentItem(),"selectedVelocity")->property("value").toInt(),69);
        QCOMPARE(findItem(window->contentItem(),"selectedGate")->property("value").toInt(),74);
        QVERIFY(window->grabWindow().save("desktop-session.png"));
        QVERIFY(window->grabWindow().save("desktop-step-editor.png"));
        click("clip0"); session.tick();
        auto* clipLabel = findItem(window->contentItem(),"clipState0");
        QVERIFY(clipLabel); QCOMPARE(clipLabel->property("text").toString(),QString("Launch queued"));
        QVERIFY(!findItem(window->contentItem(),"clip0")->property("highlighted").toBool());
        click("playButton"); session.tick(); QVERIFY(session.running());
        QCOMPARE(clipLabel->property("text").toString(),QString("Active"));
        QVERIFY(findItem(window->contentItem(),"clip0")->property("highlighted").toBool());
        QVERIFY(window->grabWindow().save("desktop-active-clip.png"));
        auto* label = window->findChild<QObject*>("transportState");
        QVERIFY(label); QCOMPARE(label->property("text").toString(), QString("Playing"));
        click("stopButton"); session.tick(); QVERIFY(!session.running());
        QCOMPARE(clipLabel->property("text").toString(),QString("Stopped"));
        QCOMPARE(label->property("text").toString(), QString("Stopped"));
    }
};
QTEST_MAIN(DesktopTests)
#include "desktop_tests.moc"
