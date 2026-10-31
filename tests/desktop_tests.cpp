#include "session.hpp"
#include <QtTest>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QtEndian>
#include <cmath>
#include <array>
#include <functional>
#include <limits>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "fl_live/document_io.hpp"

static ProjectSettings readAuthoredSettings(const QString& path) { return readAuthoredProject(path).settings; }

class DesktopTests : public QObject {
    Q_OBJECT
private slots:
    void sceneRequestsAndHistory() {
        Session session;const auto id=session.sceneId();const auto clips=session.clipIds();
        QCOMPARE(session.sceneState(),QString("Stopped"));
        QVERIFY(!session.launchScene("05","immediate"));QVERIFY(!session.pending());
        QVERIFY(!session.launchScene(id,"unknown"));QVERIFY(!session.pending());
        QVERIFY(!session.stopScene("999","one-bar"));QVERIFY(!session.pending());
        session.selectStep(2);QVERIFY(session.renameScene(id,"Verse"));QCOMPARE(session.selectedStep(),2);
        session.undo();QCOMPARE(session.sceneName(),QString("Scene 1"));session.redo();QCOMPARE(session.sceneName(),QString("Verse"));
        QTemporaryDir directory;const auto path=directory.filePath("scene.json");QVERIFY(session.saveProject(path));
        QCOMPARE(readAuthoredProject(path).identities.scenes[0].name,std::string("Verse"));
        QVERIFY(session.launchScene(id,"one-bar"));QCOMPARE(session.sceneState(),QString("Launch requested"));
        QVERIFY(!session.renameScene(id,"Pending"));session.tick();
        QCOMPARE(session.sceneState(),QString("Stopped · actions queued"));QVERIFY(!session.dirty());
        session.play();session.tick();QCOMPARE(session.sceneState(),QString("Active"));QCOMPARE(session.activeClipIds(),clips);
        QVERIFY(!session.renameScene(id,"Playing"));
        QVERIFY(session.stopScene(id,"immediate"));QCOMPARE(session.sceneState(),QString("Stop requested"));session.tick();
        QCOMPARE(session.sceneState(),QString("Stopped"));QVERIFY(session.running());
        QVERIFY(session.launchScene(id,"one-bar"));session.tick();QCOMPARE(session.sceneState(),QString("Stopped · actions queued"));
        // Immediate does not discard the queued next-bar action.
        QVERIFY(session.launchScene(id,"immediate"));session.tick();QCOMPARE(session.sceneState(),QString("Active · actions queued"));
        session.stop();session.tick();QCOMPARE(session.sceneState(),QString("Stopped"));
        QVERIFY(session.loadProject(path));QCOMPARE(session.sceneId(),id);QCOMPARE(session.sceneName(),QString("Verse"));
        for(int n=0;n<64;++n)QVERIFY(session.launchScene(id,"one-bar"));
        QVERIFY(!session.stopScene(id,"immediate"));QVERIFY(session.message().contains("queue full"));
        session.panic();session.tick();QCOMPARE(session.sceneState(),QString("Stopped"));QVERIFY(!session.pending());
    }
    void identityRenamingAndHistory() {
        Session session;session.selectStep(2);
        const auto tracks=session.trackIds(),clips=session.clipIds();
        QVERIFY(session.renameTrack(tracks[0].toString(),"Drums"));
        QVERIFY(session.renameClip(clips[1].toString(),"Melody"));
        QCOMPARE(session.trackIds(),tracks);QCOMPARE(session.clipIds(),clips);QCOMPARE(session.selectedStep(),2);
        QCOMPARE(session.trackNames()[0].toString(),QString("Drums"));
        QCOMPARE(session.clipNames()[1].toString(),QString("Melody"));
        QTemporaryDir directory;const auto path=directory.filePath("renamed.json");
        QVERIFY(session.saveProject(path));QVERIFY(!session.dirty());
        const auto saved=readAuthoredProject(path);
        QCOMPARE(saved.identities.tracks[0].name,std::string("Drums"));
        session.undo();QCOMPARE(session.clipNames()[1].toString(),QString("Step pattern"));QVERIFY(session.dirty());
        session.undo();QCOMPARE(session.trackNames()[0].toString(),QString("Pulse / Audio"));
        session.redo();session.redo();QVERIFY(!session.dirty());
        const auto undo=session.undoText();
        QVERIFY(session.renameTrack(tracks[0].toString(),"Drums"));QCOMPARE(session.undoText(),undo);
        QVERIFY(!session.renameTrack(tracks[0].toString(),""));
        QVERIFY(!session.renameClip("03","Wrong ID"));
        QCOMPARE(session.undoText(),undo);QVERIFY(!session.dirty());
        QVERIFY(session.launchClip(clips[1].toString()));session.play();
        QVERIFY(!session.renameClip(clips[1].toString(),"Pending"));
        session.tick();QVERIFY(session.running());QCOMPARE(session.activeClipIds()[1],clips[1]);
        QVERIFY(!session.renameTrack(tracks[0].toString(),"Running"));
        session.panic();session.tick();QCOMPARE(session.activeClipIds()[1].toString(),QString{});
        QVERIFY(session.loadProject(path));QCOMPARE(session.trackNames()[0].toString(),QString("Drums"));QVERIFY(!session.canUndo());
        // Test the actual desktop string-ID path above JavaScript's exact range.
        auto high=saved;constexpr std::uint64_t delta=9007199254740992ULL;
        for(auto& t:high.identities.tracks)t.id.value+=delta;
        for(auto& c:high.identities.clips){c.id.value+=delta;c.track.value+=delta;}
        for(auto& s:high.identities.scenes){s.id.value+=delta;for(auto& e:s.entries){e.track.value+=delta;e.clip.value+=delta;}}
        high.identities.next_id+=delta;
        const auto highPath=directory.filePath("high.json");writeAuthoredProject(highPath,high);
        QVERIFY(session.loadProject(highPath));
        QCOMPARE(session.trackIds()[0].toString(),QString("9007199254740993"));
        QVERIFY(session.renameTrack(session.trackIds()[0].toString(),"High ID"));
        QVERIFY(session.launchClip(session.clipIds()[0].toString()));session.play();session.tick();
        QCOMPARE(session.activeClipIds()[0],session.clipIds()[0]);session.panic();session.tick();
    }
    void qmlRenameInputAndFocus() {
        QTemporaryDir directory;auto high=AuthoredProject{{},defaultTemplateIdentities()};
        constexpr std::uint64_t delta=9007199254740992ULL;
        for(auto& t:high.identities.tracks)t.id.value+=delta;
        for(auto& c:high.identities.clips){c.id.value+=delta;c.track.value+=delta;}
        for(auto& s:high.identities.scenes){s.id.value+=delta;for(auto& e:s.entries){e.track.value+=delta;e.clip.value+=delta;}}
        high.identities.next_id+=delta;
        const auto inputPath=directory.filePath("wide-ids.json");writeAuthoredProject(inputPath,high);
        Session session;QVERIFY(session.loadProject(inputPath));session.selectStep(2);
        const auto clipId=session.clipIds()[1];
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));QCOMPARE(engine.rootObjects().size(),1);
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> find=[&](QQuickItem* item,const QString& name)->QQuickItem*{
            if(item->objectName()==name && item->isVisible())return item;
            for(auto* child:item->childItems())if(auto* result=find(child,name))return result;return nullptr;
        };
        auto click=[&](const char* name){auto* item=find(window->contentItem(),QString::fromUtf8(name));QVERIFY(item);QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());};
        auto type=[&](const char* text){for(auto c:QByteArray(text))QTest::keyClick(window,c);};
        auto* dialog=window->findChild<QObject*>("renameDialog");QVERIFY(dialog);
        click("renameTrack0");QTRY_VERIFY(dialog->property("visible").toBool());
        auto* input=window->findChild<QObject*>("renameValue");QVERIFY(input);QTRY_VERIFY(input->property("activeFocus").toBool());
        type("Drums");session.tick();
        QCOMPARE(input->property("text").toString(),QString("Drums"));
        QTest::keyClick(window,Qt::Key_Return);QTRY_COMPARE(session.trackNames()[0].toString(),QString("Drums"));
        QTRY_VERIFY(!dialog->property("visible").toBool());QTRY_VERIFY(find(window->contentItem(),"renameTrack0")->hasActiveFocus());
        QCOMPARE(session.selectedStep(),2);
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier);QTRY_COMPARE(session.trackNames()[0].toString(),QString("Pulse / Audio"));
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);QTRY_COMPARE(session.trackNames()[0].toString(),QString("Drums"));
        click("renameClip1");QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(input->property("activeFocus").toBool());type("Melody");click("applyRename");QTRY_COMPARE(session.clipNames()[1].toString(),QString("Melody"));
        QTRY_VERIFY(find(window->contentItem(),"renameClip1")->hasActiveFocus());
        const auto undo=session.undoText();
        click("renameClip1");QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(input->property("activeFocus").toBool());
        QTest::keyClick(window,Qt::Key_End);QTest::keyClick(window,Qt::Key_X);QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier);
        QCOMPARE(session.clipNames()[1].toString(),QString("Melody"));
        QTRY_COMPARE(input->property("text").toString(),QString("Melody"));QCOMPARE(session.undoText(),undo);
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);QTest::keyClick(window,Qt::Key_Backspace);QTest::keyClick(window,Qt::Key_Return);
        QVERIFY(dialog->property("visible").toBool());QCOMPARE(session.clipNames()[1].toString(),QString("Melody"));
        click("cancelRename");QTRY_VERIFY(!dialog->property("visible").toBool());
        click("renameScene");QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(input->property("activeFocus").toBool());type("Verse");QTest::keyClick(window,Qt::Key_Return);
        QTRY_COMPARE(session.sceneName(),QString("Verse"));QTRY_VERIFY(find(window->contentItem(),"renameScene")->hasActiveFocus());
        const auto path=directory.filePath("qml-renamed.json");
        auto* projectFile=window->findChild<QObject*>("projectFile");QVERIFY(projectFile);projectFile->setProperty("text",path);
        click("saveProjectButton");QVERIFY(!session.dirty());
        click("openProjectButton");QCOMPARE(session.trackNames()[0].toString(),QString("Drums"));
        QCOMPARE(session.clipNames()[1].toString(),QString("Melody"));QVERIFY(!session.canUndo());
        const auto saved=readAuthoredProject(path);QCOMPARE(saved.identities.tracks[0].name,std::string("Drums"));
        QCOMPARE(saved.identities.tracks[0].id.value,9007199254740993ULL);
        QCOMPARE(saved.identities.scenes[0].name,std::string("Verse"));QCOMPARE(session.sceneId(),QString("9007199254740997"));
        click("sceneTiming");QTest::keyClick(window,Qt::Key_Home);QTest::keyClick(window,Qt::Key_Return);
        auto* timing=find(window->contentItem(),"sceneTiming");QVERIFY(timing);QTRY_COMPARE(timing->property("currentIndex").toInt(),0);
        click("launchScene");click("playButton");session.tick();QCOMPARE(session.activeClipIds()[1],clipId);
        QCOMPARE(session.sceneState(),QString("Active"));
        auto* sceneStop=find(window->contentItem(),"stopScene");QVERIFY(sceneStop);sceneStop->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Space);session.tick();QCOMPARE(session.sceneState(),QString("Stopped"));QVERIFY(session.running());
        click("sceneTiming");QTest::keyClick(window,Qt::Key_End);QTest::keyClick(window,Qt::Key_Return);
        QTRY_COMPARE(timing->property("currentIndex").toInt(),1);click("launchScene");session.tick();
        QCOMPARE(session.sceneState(),QString("Stopped · actions queued"));
        QVERIFY(window->grabWindow().save("desktop-scene-controls.png"));
        click("stopButton");session.tick();QVERIFY(!session.running());
        QVERIFY(window->grabWindow().save("desktop-identity-renaming.png"));
    }
    void metadataIsNotMusicalProject() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        fl_live::SessionDocument metadata;
        metadata.createTrack("Metadata only", fl_live::TrackKind::audio);
        const auto path = directory.filePath("metadata.json");
        fl_live::writeDocumentMetadata(path, metadata.snapshot());
        Session session;
        session.setTempo(137);
        const auto templatePath = directory.filePath("musical.json");
        QVERIFY(session.saveProject(templatePath));
        const auto undo = session.undoText();
        QVERIFY(session.canUndo());
        QVERIFY(!session.loadProject(path));
        QCOMPARE(session.tempo(), 137);
        QCOMPARE(session.projectPath(), templatePath);
        QCOMPARE(session.undoText(), undo);
        QVERIFY(session.canUndo()); QVERIFY(!session.dirty());
        QVERIFY(session.message().contains("Unsupported project format"));
        bool rejected = false;
        try { fl_live::readDocumentMetadata(templatePath); }
        catch (const std::exception&) { rejected = true; }
        QVERIFY(rejected);
    }
    void mismatchedBackendRate() {
        fl_live::PipeWireConfig config;
        config.remote = "fl-live-unopened-rate-test";
        config.sample_rate = 44100; // prepared template remains 48000 Hz
        Session session(config);
        QVERIFY(!session.audioAvailable());
        QVERIFY(!session.running());
        QVERIFY(session.message().contains("backend rate must match prepared session"));
        QVERIFY(!session.reconnectAudio());
        session.play(); session.tick();
        QVERIFY(!session.running());
        QVERIFY(!session.pending());
        QCOMPARE(session.tempo(), 120);
        QTemporaryDir directory;
        const auto path = directory.filePath("unchanged.json");
        QVERIFY(session.saveProject(path));
        QVERIFY(readAuthoredSettings(path) == ProjectSettings{});
    }
    void missingAudioServer() {
        QTemporaryDir directory;
        fl_live::PipeWireConfig config;
        config.remote = directory.filePath("missing-pipewire").toStdString();
        Session session(config);
        QVERIFY(!session.audioAvailable());
        QVERIFY(!session.running());
        QVERIFY(!session.pending());
        QVERIFY(session.message().contains("Retry audio"));
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("session", &session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        QCOMPARE(engine.rootObjects().size(), 1);
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* retry = window->findChild<QQuickItem*>("retryAudioButton");
        auto* play = window->findChild<QQuickItem*>("playButton");
        QVERIFY(retry && retry->isVisible() && retry->isEnabled());
        QVERIFY(play && !play->isEnabled());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            retry->mapToScene(QPointF(retry->width()/2, retry->height()/2)).toPoint());
        QVERIFY(!session.audioAvailable());
        QVERIFY(retry->isVisible());
        session.play(); session.launch(0, 0); session.panic(); session.tick();
        QVERIFY(!session.running());
        QVERIFY(!session.pending());
        const auto path = directory.filePath("preserved.json");
        QVERIFY(session.saveProject(path));
        QCOMPARE(readAuthoredSettings(path).tempo, 120);
        QVERIFY(!session.reconnectAudio());
        QVERIFY(!session.audioAvailable());
        QVERIFY(!session.running());
        QVERIFY(!session.dirty());
    }
    void projectWavExport() {
        QTemporaryDir directory;
        const auto path=directory.filePath("mix.wav");
        ProjectSettings settings;settings.tempo=137;settings.levels={70,40};
        settings.step_count=1;settings.steps[0]={true,69,50,100};
        exportProjectWav(path,settings,1);
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();file.close();
        const auto frames=(48000ULL*60*4+136)/137;
        QCOMPARE(bytes.size(),static_cast<qsizetype>(44+frames*2));
        QCOMPARE(bytes.first(4),QByteArray("RIFF"));QCOMPARE(bytes.mid(8,8),QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData()+24),48000U);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData()+22),1);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData()+34),16);
        const auto plan=prepareProject(settings);fl_live::Engine engine(plan);
        QCOMPARE(engine.schedule({fl_live::CommandKind::launch,0,0,0}),fl_live::ScheduleResult::accepted);
        QCOMPARE(engine.schedule({fl_live::CommandKind::launch,0,1,0}),fl_live::ScheduleResult::accepted);engine.start();
        std::vector<float> expected(static_cast<std::size_t>(frames));engine.process(expected);
        for (std::size_t i=0;i<expected.size();++i)
            QCOMPARE(qFromLittleEndian<qint16>(bytes.constData()+44+i*2),static_cast<qint16>(std::lround(expected[i]*32767.0F)));
        QVERIFY_EXCEPTION_THROWN(exportProjectWav(path,settings,1),std::runtime_error);
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),bytes);file.close();
        const auto invalid=directory.filePath("invalid.wav");
        QVERIFY_EXCEPTION_THROWN(exportProjectWav(invalid,settings,0),std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(exportProjectWav(invalid,settings,17),std::invalid_argument);
        QVERIFY(!QFile::exists(invalid));
        settings.levels={0,0};exportProjectWav(directory.filePath("silent.wav"),settings,1);
        QFile silent(directory.filePath("silent.wav"));QVERIFY(silent.open(QIODevice::ReadOnly));silent.seek(44);
        QCOMPARE(silent.readAll(),QByteArray(static_cast<qsizetype>(frames*2),'\0'));
        Session session;session.setTempo(150);const auto projectPath=session.projectPath();const auto undo=session.undoText();
        QVERIFY(session.exportWav(directory.filePath("session.wav"),1));
        QVERIFY(session.dirty());QCOMPARE(session.projectPath(),projectPath);QCOMPARE(session.undoText(),undo);
        session.play();QVERIFY(!session.exportWav(invalid,1));session.tick();
        QVERIFY(!session.exportWav(invalid,1));QVERIFY(!QFile::exists(invalid));
        session.stop();session.tick();QVERIFY(session.exportWav(invalid,1));
    }
    void historyAndSavedState() {
        QTemporaryDir directory;
        Session session;
        QVERIFY(!session.canUndo());QVERIFY(!session.canRedo());QVERIFY(!session.dirty());
        session.setTempo(130);session.setLevel(0,40);session.setStepCount(8);session.setPitch(4,48);
        session.setStepEnabled(4,true);session.setVelocity(4,50);session.setGate(4,100);session.selectStep(7);
        QCOMPARE(session.undoText(),QString("Undo step 5 gate"));
        const auto path=directory.filePath("saved.json");QVERIFY(session.saveProject(path));QVERIFY(!session.dirty());
        QVERIFY(session.canUndo());
        session.undo();QCOMPARE(session.gates()[4].toInt(),75);QVERIFY(session.dirty());QVERIFY(session.canRedo());
        session.redo();QCOMPARE(session.gates()[4].toInt(),100);QVERIFY(!session.dirty());
        for (int n=0;n<7;++n) session.undo();
        QVERIFY(!session.canUndo());QVERIFY(session.canRedo());QVERIFY(session.dirty());
        QCOMPARE(session.tempo(),120);QCOMPARE(session.levels()[0].toInt(),70);QCOMPARE(session.stepCount(),4);
        QCOMPARE(session.pitches()[4].toInt(),60);QVERIFY(!session.stepEnabled()[4].toBool());
        QCOMPARE(session.velocities()[4].toInt(),70);QCOMPARE(session.gates()[4].toInt(),75);QCOMPARE(session.selectedStep(),3);
        for (int n=0;n<7;++n) session.redo();
        QVERIFY(!session.canRedo());QVERIFY(!session.dirty());QCOMPARE(session.stepCount(),8);
        const auto label=session.undoText();session.setGate(4,100);session.setGate(4,0);session.setTempo(999);
        QCOMPARE(session.undoText(),label);QVERIFY(!session.dirty());
        session.undo();const auto other=directory.filePath("other.json");QVERIFY(session.saveProject(other));
        QVERIFY(session.canRedo());session.redo();QVERIFY(session.dirty());session.undo();QVERIFY(!session.dirty());
        session.setPitch(4,49);QVERIFY(!session.canRedo());QVERIFY(session.dirty());
        session.undo();QCOMPARE(session.pitches()[4].toInt(),48);QVERIFY(!session.dirty());
        session.setLevel(0,41);QVERIFY(session.dirty());session.setLevel(0,40);QVERIFY(!session.dirty());
        session.undo();QVERIFY(session.dirty());session.redo();QVERIFY(!session.dirty());
        QCOMPARE(readAuthoredSettings(path).steps[4].gate,100);QCOMPARE(readAuthoredSettings(other).steps[4].gate,75);
    }
    void historyFailureAndDocumentBoundary() {
        Session session;session.setTempo(120);session.setTempo(0);QVERIFY(!session.canUndo());
        session.setTempo(121);session.play();QVERIFY(!session.canUndo());session.undo();QCOMPARE(session.tempo(),121);
        session.tick();session.undo();QCOMPARE(session.tempo(),121);QVERIFY(session.running());
        session.stop();session.tick();session.undo();QCOMPARE(session.tempo(),120);QVERIFY(!session.dirty());
        session.redo();QCOMPARE(session.tempo(),121);
        QTemporaryDir directory;const auto good=directory.filePath("good.json");QVERIFY(session.saveProject(good));
        session.setTempo(122);QVERIFY(!session.saveProject(directory.filePath("missing/file.json")));QVERIFY(session.dirty());
        session.undo();QCOMPARE(session.tempo(),121);QVERIFY(!session.dirty());QVERIFY(session.canRedo());
        QFile bad(directory.filePath("bad.json"));QVERIFY(bad.open(QIODevice::WriteOnly));bad.write("broken");bad.close();
        QVERIFY(!session.loadProject(bad.fileName()));QVERIFY(session.canRedo());
        session.redo();QCOMPARE(session.tempo(),122);QVERIFY(!session.loadProject(good));QVERIFY(session.canUndo());
        session.undo();QVERIFY(session.loadProject(good));QVERIFY(!session.canUndo());QVERIFY(!session.canRedo());QVERIFY(!session.dirty());
        session.setTempo(122);session.launch(0,0);session.tick();QCOMPARE(session.clipStates()[0].toString(),QString("Launch queued"));
        session.undo();QCOMPARE(session.tempo(),121);QCOMPARE(session.position(),QString("1.1"));
        QCOMPARE(session.clipStates()[0].toString(),QString("Stopped"));QVERIFY(!session.dirty());
    }
    void boundedHistory() {
        Session session;
        for (int n=0;n<130;++n) session.setTempo(n%2 ? 122 : 121);
        int undos=0;
        while (session.canUndo() && undos<130) {session.undo();++undos;}
        QCOMPARE(undos,128);QVERIFY(!session.canUndo());QCOMPARE(session.tempo(),122);QVERIFY(session.dirty());
        int redos=0;
        while (session.canRedo() && redos<130) {session.redo();++redos;}
        QCOMPARE(redos,128);QCOMPARE(session.tempo(),122);
        session.setTempo(120);QVERIFY(!session.dirty());QVERIFY(!session.canRedo());
    }
    void legacyMigration() {
        QTemporaryDir directory;
        const auto path=directory.filePath("legacy.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"format":"fl-live-project","version":1,"template":"pulse-steps-v1","tempo":120,"levels":[70,25],"pitches":[60,64,67,72]})");file.close();
        const auto settings=readAuthoredSettings(path);
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
        QVERIFY(readAuthoredSettings(converted)==settings);
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
        QFile file(good);QVERIFY(file.open(QIODevice::ReadOnly));const auto document=QJsonDocument::fromJson(file.readAll()).object()["settings"].toObject();file.close();
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
        const auto decoded = readAuthoredSettings(path);
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
        session.setTempo(160); QVERIFY(session.saveProject(path)); QCOMPARE(readAuthoredSettings(path).tempo,160);
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
        click("undoButton");QCOMPARE(session.tempo(),120);QVERIFY(!session.dirty());
        click("redoButton");QCOMPARE(session.tempo(),121);
        findItem(window->contentItem(),"undoButton")->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier);QTRY_COMPARE(session.tempo(),120);
        findItem(window->contentItem(),"redoButton")->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);QTRY_COMPARE(session.tempo(),121);
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
        auto* exportFile=findItem(window->contentItem(),"exportFile");QVERIFY(exportFile);
        exportFile->setProperty("text",directory.filePath("qml.wav"));
        click("exportWavButton");QVERIFY(QFile::exists(directory.filePath("qml.wav")));QVERIFY(session.dirty());
        auto* projectFile = window->findChild<QObject*>("projectFile");
        QVERIFY(projectFile); projectFile->setProperty("text",directory.filePath("qml.json"));
        findItem(window->contentItem(),"projectFile")->forceActiveFocus();
        session.tick();QCOMPARE(projectFile->property("text").toString(),directory.filePath("qml.json"));
        QTest::keyClick(window,Qt::Key_End);QTest::keyClick(window,Qt::Key_X);
        session.tick();QCOMPARE(projectFile->property("text").toString(),directory.filePath("qml.json")+"x");
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier);
        QTRY_COMPARE(projectFile->property("text").toString(),directory.filePath("qml.json"));
        QCOMPARE(session.gates()[4].toInt(),74); // path text undo must not undo music
        QVERIFY(!window->close()); // dirty close must leave the window open
        auto* dialog = window->findChild<QObject*>("closeDialog");
        QVERIFY(dialog); QVERIFY(dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog,"close"));
        click("saveProjectButton"); QVERIFY(!session.dirty());
        const auto saved=readAuthoredSettings(directory.filePath("qml.json"));QCOMPARE(saved.tempo,121);
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
        session.setTempo(122);session.setTempo(123);session.undo();
        QVERIFY(session.canUndo());QVERIFY(session.canRedo());QVERIFY(session.dirty());
        QVERIFY(window->grabWindow().save("desktop-history.png"));
        session.undo();QCOMPARE(session.tempo(),121);QVERIFY(!session.dirty());
    }
};
QTEST_MAIN(DesktopTests)
#include "desktop_tests.moc"
