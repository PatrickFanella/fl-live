#include "session.hpp"
#include <QtTest>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QtEndian>
#include <QFileInfo>
#include <cmath>
#include <csignal>
#include <sys/resource.h>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "fl_live/document_io.hpp"

static ProjectSettings readAuthoredSettings(const QString& path) { const auto p = readAuthoredProject(path); return templateView(p.content, p.identities); }

static void writeWav(const QString& path, int channels, int rate, const std::vector<qint16>& samples) {
    QByteArray bytes("RIFF");
    auto u32=[&](quint32 v){char b[4];qToLittleEndian(v,b);bytes.append(b,4);};
    auto u16=[&](quint16 v){char b[2];qToLittleEndian(v,b);bytes.append(b,2);};
    const auto data=static_cast<quint32>(samples.size()*2);
    u32(36+data);bytes.append("WAVEfmt ");u32(16);u16(1);u16(static_cast<quint16>(channels));u32(static_cast<quint32>(rate));
    u32(static_cast<quint32>(rate*channels*2));u16(static_cast<quint16>(channels*2));u16(16);bytes.append("data");u32(data);
    for(auto v:samples)u16(static_cast<quint16>(v));
    QFile file(path);if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size())throw std::runtime_error("fixture");
}
// Decoded stereo PCM16 frames of an exported WAV.
static std::vector<std::array<qint16,2>> readStereo(const QString& path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("export missing");
    const auto bytes=file.readAll();std::vector<std::array<qint16,2>> frames;
    for(qsizetype at=44;at+3<bytes.size();at+=4)
        frames.push_back({qFromLittleEndian<qint16>(bytes.constData()+at),qFromLittleEndian<qint16>(bytes.constData()+at+2)});
    return frames;
}
// Independent per-track render of one clip at full gain, center pan.
static std::vector<float> renderClip(const fl_live::Clip& clip,int tempo,std::size_t frames) {
    fl_live::SessionPlan plan{48000,static_cast<std::uint32_t>(tempo*1000),{{1.0F,{clip}}}};
    fl_live::Engine engine(plan);
    if(engine.schedule({fl_live::CommandKind::launch,0,0,0})!=fl_live::ScheduleResult::accepted)throw std::runtime_error("schedule");
    engine.start();std::vector<float> stereo(frames*2);(void)engine.process(stereo,2);
    std::vector<float> left(frames);for(std::size_t i=0;i<frames;++i)left[i]=stereo[2*i];return left;
}

class DesktopTests : public QObject {
    Q_OBJECT
private slots:
    void copyAndReorderScenes() {
        Session session;const auto first=session.sceneId();const auto tracks=session.trackIds(),clips=session.clipIds();
        QVERIFY(session.setSceneIncluded(tracks[0].toString(),false));
        QTemporaryDir directory;const auto path=directory.filePath("copies.json");QVERIFY(session.saveProject(path));
        QVERIFY(!session.copyScene("05"));QVERIFY(!session.copyScene("999"));
        QVERIFY(!session.moveScene(first,-1));QVERIFY(!session.moveScene(first,0));QVERIFY(!session.dirty());
        QVERIFY(session.copyScene(first));const auto copy=session.sceneId();QVERIFY(copy!=first);
        QCOMPARE(session.sceneIncluded(),(QVariantList{false,true}));QCOMPARE(session.clipIds(),clips);QCOMPARE(session.trackIds(),tracks);
        session.undo();QCOMPARE(session.sceneId(),first);QVERIFY(!session.dirty());session.redo();QCOMPARE(session.sceneId(),copy);
        QVERIFY(session.setSceneIncluded(tracks[0].toString(),true));
        QVERIFY(session.selectScene(first));QCOMPARE(session.sceneIncluded(),(QVariantList{false,true}));
        QVERIFY(session.moveScene(copy,-1));QCOMPARE(session.sceneId(),copy);QCOMPARE(session.selectedSceneIndex(),0);
        QCOMPARE(session.sceneIds(),(QVariantList{copy,first}));session.undo();QCOMPARE(session.sceneId(),first);
        QCOMPARE(session.sceneIds(),(QVariantList{first,copy}));session.redo();QCOMPARE(session.sceneId(),copy);
        QVERIFY(!session.moveScene(copy,-1));QVERIFY(!session.moveScene("999",1));
        QVERIFY(session.saveProject(path));const auto saved=readAuthoredProject(path);QCOMPARE(saved.identities.clips.size(),std::size_t(2));
        QCOMPARE(saved.identities.scenes[0].id.value,copy.toULongLong());QCOMPARE(saved.identities.scenes[0].entries.size(),std::size_t(2));
        QCOMPARE(saved.identities.scenes[1].entries.size(),std::size_t(1));
        QVERIFY(session.loadProject(path));QCOMPARE(session.sceneId(),copy);
        QVERIFY(session.launchScene(first,"immediate"));session.play();
        QVERIFY(!session.copyScene(first));QVERIFY(!session.moveScene(copy,1));session.tick();
        QCOMPARE(session.activeClipIds()[0].toString(),QString{});QCOMPARE(session.activeClipIds()[1],clips[1]);
        session.stop();session.tick();QVERIFY(session.selectScene(copy));QVERIFY(session.launchScene(copy,"immediate"));session.play();session.tick();
        QCOMPARE(session.activeClipIds(),clips);session.stop();session.tick();
        // Copy uses the same high-water allocation rule as New scene.
        QVERIFY(session.copyScene(copy));const auto retired=session.sceneId();session.undo();QVERIFY(!session.dirty());
        QVERIFY(session.saveProject(path));QVERIFY(session.loadProject(path));QVERIFY(session.copyScene(copy));
        QVERIFY(session.sceneId().toULongLong()>retired.toULongLong());
    }
    void sceneManagementHistoryAndIds() {
        Session session;const auto first=session.sceneId();const auto tracks=session.trackIds();
        QVERIFY(!session.deleteScene(first));QVERIFY(!session.dirty());
        QVERIFY(session.createScene());const auto second=session.sceneId();QVERIFY(second!=first);
        QCOMPARE(session.sceneIncluded(),(QVariantList{false,false}));QCOMPARE(session.selectedSceneIndex(),1);
        QVERIFY(session.renameScene(second,"Break"));QVERIFY(session.setSceneIncluded(tracks[1].toString(),true));
        QTemporaryDir directory;const auto path=directory.filePath("arrangements.json");QVERIFY(session.saveProject(path));
        QVERIFY(session.selectScene(first));QVERIFY(!session.dirty());QCOMPARE(session.sceneIncluded(),(QVariantList{true,true}));
        QVERIFY(!session.selectScene("999"));QCOMPARE(session.sceneId(),first);
        QVERIFY(session.selectScene(second));QVERIFY(session.createScene());const auto third=session.sceneId();
        QVERIFY(session.deleteScene(third));QCOMPARE(session.sceneId(),second);
        session.undo();QCOMPARE(session.sceneId(),third);session.redo();QCOMPARE(session.sceneId(),second);
        session.undo();session.undo();QCOMPARE(session.sceneId(),second);QVERIFY(!session.dirty());
        // Undo cannot recycle the third identity, including after saving/reopening.
        QVERIFY(session.saveProject(path));const auto cursor=readAuthoredProject(path).identities.next_id;
        QVERIFY(cursor>third.toULongLong());QVERIFY(session.loadProject(path));
        QCOMPARE(session.sceneId(),first);QVERIFY(session.createScene());QVERIFY(session.sceneId().toULongLong()>=cursor);
        const auto fresh=session.sceneId();session.undo();QVERIFY(session.createScene());QVERIFY(session.sceneId().toULongLong()>fresh.toULongLong());
        QVERIFY(!session.canRedo());
        const auto empty=session.sceneId();QVERIFY(session.launchScene(first,"immediate"));session.play();
        QVERIFY(!session.createScene());QVERIFY(!session.deleteScene(empty));
        QVERIFY(session.selectScene(empty));QVERIFY(!session.sceneState().contains("requested"));session.tick();
        QVERIFY(session.launchScene(empty,"immediate"));session.tick();QCOMPARE(session.sceneState(),QString("Stopped"));
        session.stop();session.tick();
        auto capped=defaultProject();fl_live::SessionDocument document(capped.identities);
        for(int i=1;i<256;++i)document.createScene("Scene");capped.identities=document.snapshot();
        const auto capPath=directory.filePath("limit.json");writeAuthoredProject(capPath,capped);
        Session limit;QVERIFY(limit.loadProject(capPath));QVERIFY(!limit.createScene());QVERIFY(!limit.dirty());QVERIFY(!limit.canUndo());
        QCOMPARE(limit.sceneIds().size(),256);
        QVERIFY(!limit.copyScene(limit.sceneId()));QVERIFY(!limit.dirty());QVERIFY(!limit.canUndo());
    }
    void qmlSceneManagementInput() {
        Session session;const auto initial=session.sceneId();
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> find=[&](QQuickItem* item,const QString& name)->QQuickItem*{
            if(item->objectName()==name && item->isVisible())return item;
            for(auto* child:item->childItems())if(auto* result=find(child,name))return result;return nullptr;
        };
        auto click=[&](const char* name){auto* item=find(window->contentItem(),name);QVERIFY(item);QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());};
        click("createScene");QCOMPARE(session.sceneIds().size(),2);const auto second=session.sceneId();
        QCOMPARE(session.sceneIncluded(),(QVariantList{false,false}));click("sceneIncluded1");
        auto* selector=find(window->contentItem(),"sceneSelector");QVERIFY(selector);
        click("renameScene");auto* input=window->findChild<QObject*>("renameValue");QVERIFY(input);
        QTRY_VERIFY(input->property("activeFocus").toBool());for(auto c:QByteArray("Break"))QTest::keyClick(window,c);
        QTest::keyClick(window,Qt::Key_Return);QTRY_COMPARE(session.sceneName(),QString("Break"));
        QTRY_COMPARE(selector->property("currentText").toString(),QString("Break"));QCOMPARE(selector->property("currentIndex").toInt(),1);
        click("sceneSelector");QSignalSpy documents(&session,&Session::documentChanged);session.tick();QCOMPARE(documents.count(),0);
        QTest::keyClick(window,Qt::Key_Home);QTest::keyClick(window,Qt::Key_Return);QTRY_COMPARE(session.sceneId(),initial);
        selector->forceActiveFocus();QTest::keyClick(window,Qt::Key_End);QTRY_COMPARE(session.sceneId(),second);
        QCOMPARE(session.sceneIncluded(),(QVariantList{false,true}));
        click("copyScene");const auto copied=session.sceneId();QVERIFY(copied!=second);QCOMPARE(session.sceneIds().size(),3);
        click("sceneUp");QCOMPARE(session.sceneIds(),(QVariantList{initial,copied,second}));
        click("undoButton");QCOMPARE(session.sceneIds(),(QVariantList{initial,second,copied}));
        auto* up=find(window->contentItem(),"sceneUp");QVERIFY(up);up->forceActiveFocus();QTest::keyClick(window,Qt::Key_Space);
        QCOMPARE(session.sceneIds(),(QVariantList{initial,copied,second}));
        click("sceneDown");QCOMPARE(session.sceneIds(),(QVariantList{initial,second,copied}));
        click("deleteScene");QCOMPARE(session.sceneId(),second);QCOMPARE(session.sceneIds().size(),2);
        click("deleteScene");QCOMPARE(session.sceneId(),initial);QCOMPARE(session.sceneIds().size(),1);
        click("undoButton");QCOMPARE(session.sceneId(),second);QCOMPARE(session.sceneIds().size(),2);
        click("redoButton");QCOMPARE(session.sceneId(),initial);click("undoButton");
        QTemporaryDir directory;auto* file=window->findChild<QObject*>("projectFile");QVERIFY(file);file->setProperty("text",directory.filePath("scenes.json"));
        click("saveProjectButton");click("openProjectButton");QCOMPARE(session.sceneIds().size(),2);QCOMPARE(session.sceneId(),initial);
        click("sceneSelector");QTest::keyClick(window,Qt::Key_End);QTest::keyClick(window,Qt::Key_Return);QTRY_COMPARE(session.sceneId(),second);
        click("launchScene");click("playButton");session.tick();QCOMPARE(session.sceneState(),QString("Active"));
        QCOMPARE(session.activeClipIds()[0].toString(),QString{});QCOMPARE(session.activeClipIds()[1],session.clipIds()[1]);
        QVERIFY(window->grabWindow().save("desktop-scene-management.png"));click("stopButton");session.tick();
        QVERIFY(window->grabWindow().save("desktop-scene-copy-order.png"));
    }
    void sceneSlotHistoryAndPlayback() {
        Session session;session.selectStep(2);const auto tracks=session.trackIds(),clips=session.clipIds();
        const auto id=session.sceneId();QCOMPARE(session.sceneIncluded(),(QVariantList{true,true}));
        QVERIFY(!session.setSceneIncluded("01",false));QVERIFY(!session.dirty());
        QVERIFY(!session.setSceneIncluded("999",false));QVERIFY(!session.canUndo());
        QVERIFY(session.setSceneIncluded(tracks[1].toString(),false));
        QCOMPARE(session.selectedStep(),2);QCOMPARE(session.sceneIncluded(),(QVariantList{true,false}));
        const auto undo=session.undoText();QVERIFY(session.setSceneIncluded(tracks[1].toString(),false));QCOMPARE(session.undoText(),undo);
        QTemporaryDir directory;const auto path=directory.filePath("sparse.json");QVERIFY(session.saveProject(path));
        session.undo();QCOMPARE(session.sceneIncluded(),(QVariantList{true,true}));QVERIFY(session.dirty());
        session.redo();QCOMPARE(session.sceneIncluded(),(QVariantList{true,false}));QVERIFY(!session.dirty());
        QVERIFY(session.launchClip(clips[1].toString()));session.play();
        QVERIFY(!session.setSceneIncluded(tracks[0].toString(),false));session.tick();
        QCOMPARE(session.sceneState(),QString("Partial"));
        QVERIFY(session.launchScene(id,"immediate"));session.tick();
        QCOMPARE(session.activeClipIds()[0],clips[0]);QCOMPARE(session.activeClipIds()[1].toString(),QString{});
        QCOMPARE(session.sceneState(),QString("Active"));
        session.stop();session.tick();QVERIFY(session.loadProject(path));
        QCOMPARE(session.sceneIncluded(),(QVariantList{true,false}));QVERIFY(!session.canUndo());
        QVERIFY(session.setSceneIncluded(tracks[0].toString(),false));
        QVERIFY(session.launchScene(id,"immediate"));session.play();session.tick();
        QCOMPARE(session.sceneState(),QString("Stopped"));QVERIFY(session.running());
        QCOMPARE(session.activeClipIds(),(QVariantList{QString{},QString{}}));
        session.stop();session.tick();QVERIFY(session.setSceneIncluded(tracks[1].toString(),true));
        QVERIFY(session.launchScene(id,"one-bar"));session.play();session.tick();
        QCOMPARE(session.activeClipIds()[1],clips[1]);QCOMPARE(session.activeClipIds()[0].toString(),QString{});
        QCOMPARE(session.sceneState(),QString("Active"));session.panic();session.tick();
    }
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
        for(auto& t:high.content.tracks)t.track.value+=delta;for(auto& c:high.content.clips)c.clip.value+=delta;
        const auto highPath=directory.filePath("high.json");writeAuthoredProject(highPath,high);
        QVERIFY(session.loadProject(highPath));
        QCOMPARE(session.trackIds()[0].toString(),QString("9007199254740993"));
        QVERIFY(session.renameTrack(session.trackIds()[0].toString(),"High ID"));
        QVERIFY(session.launchClip(session.clipIds()[0].toString()));session.play();session.tick();
        QCOMPARE(session.activeClipIds()[0],session.clipIds()[0]);session.panic();session.tick();
    }
    void qmlRenameInputAndFocus() {
        QTemporaryDir directory;auto high=defaultProject();
        constexpr std::uint64_t delta=9007199254740992ULL;
        for(auto& t:high.identities.tracks)t.id.value+=delta;
        for(auto& c:high.identities.clips){c.id.value+=delta;c.track.value+=delta;}
        for(auto& s:high.identities.scenes){s.id.value+=delta;for(auto& e:s.entries){e.track.value+=delta;e.clip.value+=delta;}}
        high.identities.next_id+=delta;high.content=migrateTemplate(ProjectSettings{},high.identities);
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
        click("sceneIncluded1");QCOMPARE(session.sceneIncluded(),(QVariantList{true,false}));
        auto* include=find(window->contentItem(),"sceneIncluded1");QVERIFY(include);include->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Space);QCOMPARE(session.sceneIncluded(),(QVariantList{true,true}));
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier);QTRY_COMPARE(session.sceneIncluded(),(QVariantList{true,false}));
        click("saveProjectButton");click("openProjectButton");QCOMPARE(session.sceneIncluded(),(QVariantList{true,false}));
        click("sceneTiming");QTest::keyClick(window,Qt::Key_Home);QTest::keyClick(window,Qt::Key_Return);
        auto* timing=find(window->contentItem(),"sceneTiming");QVERIFY(timing);QTRY_COMPARE(timing->property("currentIndex").toInt(),0);
        click("clip1");click("playButton");session.tick();QCOMPARE(session.activeClipIds()[1],clipId);
        click("launchScene");session.tick();QCOMPARE(session.activeClipIds()[1].toString(),QString{});
        QCOMPARE(session.sceneState(),QString("Active"));
        auto* sceneStop=find(window->contentItem(),"stopScene");QVERIFY(sceneStop);sceneStop->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Space);session.tick();QCOMPARE(session.sceneState(),QString("Stopped"));QVERIFY(session.running());
        click("sceneTiming");QTest::keyClick(window,Qt::Key_End);QTest::keyClick(window,Qt::Key_Return);
        QTRY_COMPARE(timing->property("currentIndex").toInt(),1);click("launchScene");session.tick();
        QCOMPARE(session.sceneState(),QString("Stopped · actions queued"));
        QVERIFY(window->grabWindow().save("desktop-scene-controls.png"));
        QVERIFY(window->grabWindow().save("desktop-scene-slots.png"));
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
        const auto identities=defaultTemplateIdentities();
        exportProjectWav(path,migrateTemplate(settings,identities),identities,1);
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();file.close();
        const auto frames=(48000ULL*60*4+136)/137;
        QCOMPARE(bytes.size(),static_cast<qsizetype>(44+frames*4));
        QCOMPARE(bytes.first(4),QByteArray("RIFF"));QCOMPARE(bytes.mid(8,8),QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData()+24),48000U);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData()+22),2);
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData()+28),192000U);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData()+32),4);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData()+34),16);
        // Centered tracks: both channels equal the original mono template mix.
        const auto plan=prepareProject(settings);fl_live::Engine engine(plan);
        QCOMPARE(engine.schedule({fl_live::CommandKind::launch,0,0,0}),fl_live::ScheduleResult::accepted);
        QCOMPARE(engine.schedule({fl_live::CommandKind::launch,0,1,0}),fl_live::ScheduleResult::accepted);engine.start();
        std::vector<float> expected(static_cast<std::size_t>(frames));engine.process(expected);
        for (std::size_t i=0;i<expected.size();++i) {
            const auto value=static_cast<qint16>(std::lround(expected[i]*32767.0F));
            QCOMPARE(qFromLittleEndian<qint16>(bytes.constData()+44+i*4),value);
            QCOMPARE(qFromLittleEndian<qint16>(bytes.constData()+46+i*4),value);
        }
        QVERIFY_EXCEPTION_THROWN(exportProjectWav(path,migrateTemplate(settings,identities),identities,1),std::runtime_error);
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),bytes);file.close();
        const auto invalid=directory.filePath("invalid.wav");
        QVERIFY_EXCEPTION_THROWN(exportProjectWav(invalid,migrateTemplate(settings,identities),identities,0),std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(exportProjectWav(invalid,migrateTemplate(settings,identities),identities,17),std::invalid_argument);
        QVERIFY(!QFile::exists(invalid));
        settings.levels={0,0};exportProjectWav(directory.filePath("silent.wav"),migrateTemplate(settings,identities),identities,1);
        QFile silent(directory.filePath("silent.wav"));QVERIFY(silent.open(QIODevice::ReadOnly));silent.seek(44);
        QCOMPARE(silent.readAll(),QByteArray(static_cast<qsizetype>(frames*4),'\0'));
        Session session;session.setTempo(150);const auto projectPath=session.projectPath();const auto undo=session.undoText();
        QVERIFY(session.exportWav(directory.filePath("session.wav"),1));QTRY_VERIFY(!session.exporting());
        QVERIFY(QFile::exists(directory.filePath("session.wav")));QVERIFY(!QFile::exists(directory.filePath("session.wav.part")));
        QVERIFY(session.dirty());QCOMPARE(session.projectPath(),projectPath);QCOMPARE(session.undoText(),undo);
        session.play();QVERIFY(!session.exportWav(invalid,1));session.tick();
        QVERIFY(!session.exportWav(invalid,1));QVERIFY(!QFile::exists(invalid));
        session.stop();session.tick();QVERIFY(session.exportWav(invalid,1));QTRY_VERIFY(!session.exporting());QVERIFY(QFile::exists(invalid));
    }
    void mixerControlsAndExport() {
        // Independent per-track mono renders, mixed with the declared balance
        // law and mute/solo policy, are compared with each exported channel.
        ProjectSettings settings;settings.tempo=150;settings.levels={60,30};settings.step_count=2;
        const auto trackRender=[&](std::size_t track,std::size_t frames) {
            auto plan=prepareProject(settings);auto one=plan;one.tracks={plan.tracks[track]};
            fl_live::Engine engine(one);
            if(engine.schedule({fl_live::CommandKind::launch,0,0,0})!=fl_live::ScheduleResult::accepted)throw std::runtime_error("schedule");
            engine.start();std::vector<float> out(frames);engine.process(out);return out;
        };
        const auto pcm=[](float v){return static_cast<qint16>(std::lround(std::clamp(v,-1.0F,1.0F)*32767.0F));};
        const auto check=[&](Session& session,const QString& path,std::array<float,2> left,std::array<float,2> right) {
            QVERIFY(session.exportWav(path,1));QTRY_VERIFY(!session.exporting());
            QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();
            const auto frames=static_cast<std::size_t>((bytes.size()-44)/4);
            const auto a=trackRender(0,frames),m=trackRender(1,frames);
            for(std::size_t i=0;i<frames;++i) {
                QCOMPARE(qFromLittleEndian<qint16>(bytes.constData()+44+i*4),pcm(left[0]*a[i]+left[1]*m[i]));
                QCOMPARE(qFromLittleEndian<qint16>(bytes.constData()+46+i*4),pcm(right[0]*a[i]+right[1]*m[i]));
            }
        };
        QTemporaryDir directory;
        Session session;session.setTempo(150);session.setLevel(0,60);session.setLevel(1,30);session.setStepCount(2);
        const auto audio=session.trackIds()[0].toString(),midi=session.trackIds()[1].toString();
        check(session,directory.filePath("center.wav"),{1,1},{1,1});
        QVERIFY(session.setPan(audio,-100));QVERIFY(session.setPan(midi,40));
        QCOMPARE(session.pans(),(QVariantList{-100,40}));QCOMPARE(session.undoText(),QString("Undo track 2 pan"));
        check(session,directory.filePath("pan.wav"),{1,0.6F},{0,1});
        QVERIFY(session.setMute(audio,true));check(session,directory.filePath("mute.wav"),{0,0.6F},{0,1});
        QVERIFY(session.setSolo(audio,true));check(session,directory.filePath("mute-beats-solo.wav"),{0,0},{0,0});
        QVERIFY(session.setMute(audio,false));check(session,directory.filePath("solo.wav"),{1,0},{0,0});
        QVERIFY(session.setSolo(midi,true));check(session,directory.filePath("two-solos.wav"),{1,0.6F},{0,1});
        QCOMPARE(session.solos(),(QVariantList{true,true}));QCOMPARE(session.mutes(),(QVariantList{false,false}));
        // Refusals keep state and history.
        const auto undo=session.undoText();
        QVERIFY(!session.setPan(audio,101));QVERIFY(!session.setPan("999",0));QVERIFY(!session.setMute("x",true));
        QCOMPARE(session.pans(),(QVariantList{-100,40}));QCOMPARE(session.undoText(),undo);
        QVERIFY(session.setPan(audio,-100));QCOMPARE(session.undoText(),undo); // no-op edit adds no history
        session.play();QVERIFY(!session.setSolo(audio,false));session.tick();QVERIFY(!session.setSolo(audio,false));
        session.stop();session.tick();
        // Undo walks back each mixer step; Save/Open retains IDs and values.
        session.undo();QCOMPARE(session.solos(),(QVariantList{true,false}));
        session.undo();QCOMPARE(session.mutes(),(QVariantList{true,false}));
        session.redo();session.redo();QCOMPARE(session.solos(),(QVariantList{true,true}));
        const auto path=directory.filePath("mixer.json");QVERIFY(session.saveProject(path));QVERIFY(!session.dirty());
        const auto saved=readAuthoredProject(path);
        QCOMPARE(saved.identities.tracks[0].mix,(fl_live::TrackMix{-100,false,true}));
        QCOMPARE(saved.identities.tracks[1].mix,(fl_live::TrackMix{40,false,true}));
        QFile raw(path);QVERIFY(raw.open(QIODevice::ReadOnly));QVERIFY(raw.readAll().contains("track-mixer-v1"));
        Session reopened;QVERIFY(reopened.loadProject(path));
        QCOMPARE(reopened.pans(),(QVariantList{-100,40}));QCOMPARE(reopened.solos(),(QVariantList{true,true}));
        QCOMPARE(reopened.trackIds(),session.trackIds());QVERIFY(!reopened.dirty());
        check(reopened,directory.filePath("reopened.wav"),{1,0.6F},{0,1});
        reopened.setSolo(audio,false);reopened.setSolo(midi,false);reopened.setPan(audio,0);reopened.setPan(midi,0);
        QVERIFY(reopened.dirty());
        // Back at defaults the file is identity-only again.
        const auto plain=directory.filePath("plain.json");QVERIFY(reopened.saveProject(plain));
        QFile plainFile(plain);QVERIFY(plainFile.open(QIODevice::ReadOnly));QVERIFY(!plainFile.readAll().contains("track-mixer-v1"));
    }
    void exportCancellationAndFailures() {
        QTemporaryDir directory;
        const auto dest=directory.filePath("long.wav"),part=directory.filePath("long.wav.part");
        // Actual UI cancellation of a long export (16 bars at 20 BPM, 192 s).
        Session session;session.setTempo(20);session.setPitch(0,50);
        const auto undo=session.undoText();const auto projectPath=session.projectPath();
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* root,const QString& name)->QQuickItem* {
            if(root->objectName()==name && root->isVisible())return root;
            for(auto* child:root->childItems())if(auto* match=findItem(child,name))return match;
            return nullptr;
        };
        auto click=[&](const char* name) {
            auto* item=findItem(window->contentItem(),QString::fromUtf8(name));QVERIFY2(item,name);
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());
        };
        findItem(window->contentItem(),"exportFile")->setProperty("text",dest);
        findItem(window->contentItem(),"exportBars")->setProperty("value",16);
        click("exportWavButton");
        QVERIFY(session.exporting());QTRY_VERIFY(session.exportProgress()>0);QVERIFY(QFile::exists(part));
        QVERIFY(findItem(window->contentItem(),"exportProgress"));QVERIFY(!findItem(window->contentItem(),"exportWavButton"));
        // Editing and transport stay available; the job keeps its own copy.
        session.setTempo(21);QCOMPARE(session.tempo(),21);session.undo();QCOMPARE(session.tempo(),20);
        QVERIFY(!session.exportWav(directory.filePath("second.wav"),1));
        QVERIFY(session.message().contains("already running"));
        QVERIFY(window->grabWindow().save("desktop-export-progress.png"));
        click("cancelExportButton");
        QTRY_VERIFY(!session.exporting());
        QCOMPARE(session.message(),QString("Export cancelled; no file was written"));
        QVERIFY(!QFile::exists(dest));QVERIFY(!QFile::exists(part));
        QCOMPARE(session.undoText(),undo);QCOMPARE(session.projectPath(),projectPath);QVERIFY(session.dirty());
        QCOMPARE(session.pitches()[0].toInt(),50);QVERIFY(!session.running());QVERIFY(session.canRedo());
        QVERIFY(findItem(window->contentItem(),"exportWavButton"));
        // Write failure: a 1 MiB file-size limit stops the export mid-stream.
        rlimit before{};QCOMPARE(getrlimit(RLIMIT_FSIZE,&before),0);
        const auto previousHandler=std::signal(SIGXFSZ,SIG_IGN);
        rlimit limited=before;limited.rlim_cur=1<<20;QCOMPARE(setrlimit(RLIMIT_FSIZE,&limited),0);
        QVERIFY(session.exportWav(dest,16));QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(),20000);
        QCOMPARE(setrlimit(RLIMIT_FSIZE,&before),0);std::signal(SIGXFSZ,previousHandler);
        QVERIFY2(session.message().contains("Cannot write WAV samples"),qPrintable(session.message()));
        QVERIFY(!QFile::exists(dest));QVERIFY(!QFile::exists(part));QCOMPARE(session.undoText(),undo);QVERIFY(session.dirty());
        // A leftover partial file from a crash is never overwritten or reused.
        {QFile stale(part);QVERIFY(stale.open(QIODevice::WriteOnly));stale.write("stale");}
        QVERIFY(session.exportWav(dest,1));QTRY_VERIFY(!session.exporting());
        QVERIFY(session.message().contains("interrupted"));QVERIFY(!QFile::exists(dest));
        {QFile stale(part);QVERIFY(stale.open(QIODevice::ReadOnly));QCOMPARE(stale.readAll(),QByteArray("stale"));}
        QVERIFY(QFile::remove(part));
        // A file that appears at the destination during export is never replaced.
        QVERIFY(session.exportWav(dest,16));QTRY_VERIFY(session.exportProgress()>0);
        {QFile other(dest);QVERIFY(other.open(QIODevice::WriteOnly|QIODevice::NewOnly));other.write("theirs");}
        QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(),30000);
        QVERIFY(session.message().contains("Another file appeared"));
        {QFile other(dest);QVERIFY(other.open(QIODevice::ReadOnly));QCOMPARE(other.readAll(),QByteArray("theirs"));}
        QVERIFY(!QFile::exists(part));
        // The exported mix is the document at the start, not later edits.
        const auto snapshot=directory.filePath("snapshot.wav"),reference=directory.filePath("reference.wav");
        session.setTempo(150);
        QVERIFY(session.exportWav(snapshot,1));session.setPitch(0,72);session.setTempo(151);QTRY_VERIFY(!session.exporting());
        session.undo();session.undo();QCOMPARE(session.tempo(),150);
        QVERIFY(session.exportWav(reference,1));QTRY_VERIFY(!session.exporting());
        QFile a(snapshot),b(reference);QVERIFY(a.open(QIODevice::ReadOnly));QVERIFY(b.open(QIODevice::ReadOnly));QCOMPARE(a.readAll(),b.readAll());
        // Closing the session cancels and joins a running export.
        const auto closing=directory.filePath("closing.wav");
        {
            Session temporary;temporary.setTempo(20);QVERIFY(temporary.exportWav(closing,16));
            QTRY_VERIFY(temporary.exportProgress()>0);
        }
        QVERIFY(!QFile::exists(closing));QVERIFY(!QFile::exists(closing+".part"));
    }
    void generalTrackManagement() {
        QTemporaryDir directory;
        // A stereo loop with distinct channels: 0.25/-0.5 then 0.125/0.75.
        const auto loop=directory.filePath("loop.wav");
        writeWav(loop,2,48000,{8192,-16384,4096,24576});
        Session session;session.setTempo(150);
        QVERIFY(session.addTrack("audio"));QCOMPARE(session.trackCount(),3);QCOMPARE(session.undoText(),QString("Undo new track"));
        QVERIFY(session.addTrack("midi"));QCOMPARE(session.trackCount(),4);
        QCOMPARE(session.trackKinds(),(QVariantList{"audio","midi","audio","midi"}));
        QCOMPARE(session.trackSources()[2].toString(),QString("Empty · import a WAV"));
        QCOMPARE(session.patternTrackIndex(),1);
        const auto ids=session.trackIds();
        QVERIFY(!session.addTrack("bus"));QVERIFY(!session.importAudio(ids[1].toString(),loop));
        QVERIFY(!session.editPattern(ids[0].toString()));QCOMPARE(session.patternTrackIndex(),1);
        // Each MIDI track has its own pattern; new patterns start as rests.
        QVERIFY(session.editPattern(ids[3].toString()));QCOMPARE(session.patternTrackIndex(),3);
        QCOMPARE(session.stepEnabled()[0].toBool(),false);
        session.setStepEnabled(0,true);session.setPitch(0,48);session.setGate(0,50);
        QVERIFY(session.editPattern(ids[1].toString()));QCOMPARE(session.pitches()[0].toInt(),60);QVERIFY(session.stepEnabled()[0].toBool());
        // Import decodes on the worker and applies as one Undo step.
        QVERIFY(session.importAudio(ids[2].toString(),loop));QVERIFY(session.importing());
        QVERIFY(!session.importAudio(ids[2].toString(),loop));
        QTRY_VERIFY(!session.importing());
        QCOMPARE(session.trackSources()[2].toString(),QString("loop.wav · 0.00 s · stereo"));
        QCOMPARE(session.undoText(),QString("Undo track 3 audio import"));
        QVERIFY(session.importAudio(ids[2].toString(),directory.filePath("absent.wav")));QTRY_VERIFY(!session.importing());
        QVERIFY(session.message().startsWith("Import failed"));QCOMPARE(session.trackSources()[2].toString(),QString("loop.wav · 0.00 s · stereo"));
        const auto rate=directory.filePath("rate.wav");writeWav(rate,1,44100,{1,2,3});
        QVERIFY(session.importAudio(ids[2].toString(),rate));QTRY_VERIFY(!session.importing());
        QVERIFY(session.message().contains("44100 Hz"));QCOMPARE(session.undoText(),QString("Undo track 3 audio import"));
        // A finished import waits for Stop before changing the document.
        session.play();session.tick();QVERIFY(session.running());
        QVERIFY(session.importAudio(ids[2].toString(),loop));
        QTest::qWait(200);session.tick();QVERIFY(session.importing());QCOMPARE(session.undoText(),QString("Undo track 3 audio import"));
        session.stop();QTRY_VERIFY(!session.importing());QVERIFY(session.message().startsWith("Imported"));
        session.undo();QCOMPARE(session.trackSources()[2].toString(),QString("loop.wav · 0.00 s · stereo"));
        // Four-track export: every channel equals independent per-track renders.
        const auto check=[&](const QString& path,bool withLoop) {
            QVERIFY(session.exportWav(path,1));QTRY_VERIFY(!session.exporting());
            const auto frames=readStereo(path);QCOMPARE(frames.size(),static_cast<std::size_t>(exportFrames(150,1)));
            const auto pulse=renderClip(pulseClip(150),150,frames.size());
            const auto steps=renderClip(patternClip(StepPattern{},150),150,frames.size());
            StepPattern bassPattern;for(auto& step:bassPattern.steps)step.enabled=false;bassPattern.steps[0]={true,48,70,50};
            const auto bass=renderClip(patternClip(bassPattern,150),150,frames.size());
            const float wavLeft[]={0.25F,0.125F},wavRight[]={-0.5F,0.75F};
            const auto pcm=[](float v){return static_cast<qint16>(std::lround(std::clamp(v,-1.0F,1.0F)*32767.0F));};
            for(std::size_t i=0;i<frames.size();++i) {
                const float common=0.7F*pulse[i]+0.25F*steps[i]+0.7F*bass[i];
                const float left=common+(withLoop ? 0.7F*wavLeft[i%2] : 0.0F),right=common+(withLoop ? 0.7F*wavRight[i%2] : 0.0F);
                if(std::abs(frames[i][0]-pcm(left))>1 || std::abs(frames[i][1]-pcm(right))>1)
                    QFAIL(qPrintable(QString("export frame %1 differs").arg(i)));
            }
        };
        check(directory.filePath("four.wav"),true);
        // Save, reopen and export again; the WAV is referenced by path.
        const auto path=directory.filePath("general.json");QVERIFY(session.saveProject(path));QVERIFY(!session.dirty());
        Session reopened;QVERIFY(reopened.loadProject(path));
        QCOMPARE(reopened.trackIds(),session.trackIds());QCOMPARE(reopened.trackSources(),session.trackSources());
        QCOMPARE(reopened.patternTrackIndex(),1);QVERIFY(reopened.editPattern(ids[3].toString()));QCOMPARE(reopened.pitches()[0].toInt(),48);
        QVERIFY(reopened.exportWav(directory.filePath("reopened.wav"),1));QTRY_VERIFY(!reopened.exporting());
        QCOMPARE(readStereo(directory.filePath("reopened.wav")),readStereo(directory.filePath("four.wav")));
        // A missing asset opens as silence with a visible message.
        QVERIFY(QFile::rename(loop,directory.filePath("moved.wav")));
        Session missing;QVERIFY(missing.loadProject(path));
        QVERIFY(missing.message().contains("missing audio"));QCOMPARE(missing.trackSources()[2].toString(),QString("Missing: loop.wav"));
        QVERIFY(!missing.dirty());
        // Duplicate creates new IDs, copies the pattern and scene slot choice.
        QVERIFY(session.setSceneIncluded(ids[3].toString(),true));
        QVERIFY(session.duplicateTrack(ids[3].toString()));QCOMPARE(session.trackCount(),5);
        const auto copy=session.trackIds()[4].toString();QVERIFY(!ids.contains(copy));
        QCOMPARE(session.trackNames()[4].toString(),QString("MIDI 4 copy"));QVERIFY(session.sceneIncluded()[4].toBool());
        QVERIFY(session.clipIds()[4]!=session.clipIds()[3]);
        QVERIFY(session.editPattern(copy));QCOMPARE(session.pitches()[0].toInt(),48);
        // Delete the edited track: the editor falls back to the first MIDI track.
        QVERIFY(session.deleteTrack(copy));QCOMPARE(session.trackCount(),4);QCOMPARE(session.patternTrackIndex(),1);
        QVERIFY(session.deleteTrack(ids[1].toString()));QCOMPARE(session.patternTrackIndex(),2);
        QVERIFY(!session.deleteTrack("999"));
        session.undo();QCOMPARE(session.trackIds()[1],ids[1]);QCOMPARE(session.trackCount(),4);
        session.undo();QCOMPARE(session.trackIds()[4].toString(),copy);
        session.redo();session.redo();QCOMPARE(session.trackCount(),3);
        // Deleting every track leaves a valid, empty, saveable project.
        for(const auto& id:session.trackIds())QVERIFY(session.deleteTrack(id.toString()));
        QCOMPARE(session.trackCount(),0);QCOMPARE(session.patternTrackIndex(),-1);
        session.setStepEnabled(0,true);QVERIFY(session.message().contains("MIDI track"));
        QVERIFY(session.saveProject(directory.filePath("empty.json")));
        Session empty;QVERIFY(empty.loadProject(directory.filePath("empty.json")));QCOMPARE(empty.trackCount(),0);
        QVERIFY(empty.launchScene(empty.sceneId(),"immediate"));
    }
    void trackLimit() {
        Session session;
        while(session.trackCount()<64)QVERIFY(session.addTrack(session.trackCount()%2 ? "midi" : "audio"));
        const auto undo=session.undoText();
        QVERIFY(!session.addTrack("audio"));QVERIFY(!session.duplicateTrack(session.trackIds()[0].toString()));
        QCOMPARE(session.trackCount(),64);QCOMPARE(session.undoText(),undo);
        QTemporaryDir directory;QVERIFY(session.saveProject(directory.filePath("full.json")));
        Session reopened;QVERIFY(reopened.loadProject(directory.filePath("full.json")));QCOMPARE(reopened.trackCount(),64);
    }
    void qmlTrackManagement() {
        QTemporaryDir directory;const auto loop=directory.filePath("drums.wav");
        std::vector<qint16> samples(48000);for(std::size_t i=0;i<samples.size();++i)samples[i]=static_cast<qint16>((i%100)*100);
        writeWav(loop,1,48000,samples);
        Session session;
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* root,const QString& name)->QQuickItem* {
            if(root->objectName()==name && root->isVisible())return root;
            for(auto* child:root->childItems())if(auto* match=findItem(child,name))return match;
            return nullptr;
        };
        auto click=[&](const QString& name) {
            auto* item=findItem(window->contentItem(),name);QVERIFY2(item,qPrintable(name));
            // Bring the control into view in the horizontal track strip first.
            auto* scroll=findItem(window->contentItem(),"trackScroll");
            if(scroll) {
                auto* flick=scroll->property("contentItem").value<QQuickItem*>();
                const auto inStrip=item->mapToItem(flick,QPointF(0,0));
                if(flick && (inStrip.x()<0 || inStrip.x()+item->width()>flick->width()))
                    flick->setProperty("contentX",std::max(0.0,flick->property("contentX").toReal()+inStrip.x()-20));
                QTest::qWait(20);
            }
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());
        };
        click("addAudioTrack");QTRY_COMPARE(session.trackCount(),3);
        click("addMidiTrack");QTRY_COMPARE(session.trackCount(),4);
        QVERIFY(findItem(window->contentItem(),"trackName3"));QCOMPARE(findItem(window->contentItem(),"trackKind3")->property("text").toString(),QString("MIDI"));
        auto* path=findItem(window->contentItem(),"audioPath2");QVERIFY(path);path->setProperty("text",loop);
        click("importAudio2");QTRY_VERIFY(!session.importing());
        QCOMPARE(session.trackSources()[2].toString(),QString("drums.wav · 1.00 s · mono"));
        click("editPattern3");QCOMPARE(session.patternTrackIndex(),3);
        QCOMPARE(findItem(window->contentItem(),"patternTitle")->property("text").toString(),QString("PATTERN · MIDI 4"));
        click("step2");click("stepEnabled");QVERIFY(session.stepEnabled()[2].toBool());
        click("duplicateTrack3");QTRY_COMPARE(session.trackCount(),5);
        QCOMPARE(session.trackNames()[4].toString(),QString("MIDI 4 copy"));
        click("deleteTrack0");QTRY_COMPARE(session.trackCount(),4);QCOMPARE(session.trackNames()[0].toString(),QString("Steps / MIDI"));
        QVERIFY(window->grabWindow().save("desktop-general-tracks.png"));
        click("undoButton");QTRY_COMPARE(session.trackCount(),5);QCOMPARE(session.trackNames()[0].toString(),QString("Pulse / Audio"));
        // Launch the imported clip through its button and play.
        click("clip2");session.tick();click("playButton");session.tick();QVERIFY(session.running());
        QTRY_COMPARE(session.activeClips()[2].toInt(),0);
        QVERIFY(!findItem(window->contentItem(),"deleteTrack2")->property("enabled").toBool());
        click("stopButton");session.tick();QVERIFY(!session.running());
    }
    void clipRangeEditing() {
        QTemporaryDir directory;const auto ramp=directory.filePath("ramp.wav");
        // 4800 mono frames (100 ms): a 480-frame ramp repeated ten times.
        std::vector<qint16> samples(4800);for(std::size_t i=0;i<samples.size();++i)samples[i]=static_cast<qint16>((i%480)*64);
        writeWav(ramp,1,48000,samples);
        Session session;QVERIFY(session.addTrack("audio"));const auto track=session.trackIds()[2].toString();
        QVERIFY(session.importAudio(track,ramp));QTRY_VERIFY(!session.importing());
        QCOMPARE(session.clipFrames()[2].toDouble(),4800.0);QCOMPARE(session.clipFrames()[0].toDouble(),0.0);
        QCOMPARE(session.clipRanges()[2],QVariant(QVariantList{0.0,4800.0,true}));
        const auto peaks=session.waveform(2,10);QCOMPARE(peaks.size(),10);
        for(const auto& peak:peaks){QCOMPARE(peak.toList()[0].toFloat(),0.0F);QCOMPARE(peak.toList()[1].toFloat(),479.0F*64/32768);}
        QVERIFY(session.waveform(0,10).isEmpty());QVERIFY(session.waveform(2,0).isEmpty());QVERIFY(session.waveform(9,10).isEmpty());
        QVERIFY(session.setClipRange(track,480,960,false));
        QCOMPARE(session.clipRanges()[2],QVariant(QVariantList{480.0,960.0,false}));QCOMPARE(session.undoText(),QString("Undo track 3 clip range"));
        const auto undo=session.undoText();
        for(const auto& bad:std::vector<std::array<double,2>>{{960,960},{900,800},{0,4801},{-1,10},{1.5,10},{0,std::nan("")}})
            QVERIFY(!session.setClipRange(track,bad[0],bad[1],true));
        QVERIFY(!session.setClipRange(session.trackIds()[0].toString(),0,10,true));
        QVERIFY(!session.setClipRange(session.trackIds()[1].toString(),0,10,true));
        QCOMPARE(session.undoText(),undo);QCOMPARE(session.clipRanges()[2],QVariant(QVariantList{480.0,960.0,false}));
        // Exported audio: the soloed track plays source frames 480..959 once.
        QVERIFY(session.setSolo(track,true));
        const auto pcm=[](float v){return static_cast<qint16>(std::lround(std::clamp(v,-1.0F,1.0F)*32767.0F));};
        const auto exported=[&](const QString& name){QVERIFY(session.exportWav(directory.filePath(name),1));QTRY_VERIFY(!session.exporting());};
        exported("once.wav");auto frames=readStereo(directory.filePath("once.wav"));
        for(std::size_t i=0;i<frames.size();++i) {
            const float source=i<480 ? static_cast<float>(i*64)/32768 : 0.0F;
            QCOMPARE(frames[i][0],pcm(0.7F*source));QCOMPARE(frames[i][1],pcm(0.7F*source));
        }
        QVERIFY(session.setClipRange(track,480,960,true));exported("loop.wav");frames=readStereo(directory.filePath("loop.wav"));
        for(std::size_t i=0;i<frames.size();i+=97)QCOMPARE(frames[i][0],pcm(0.7F*static_cast<float>((i%480)*64)/32768));
        // End at the true end of the source is stored as "to the end".
        QVERIFY(session.setClipRange(track,4000,4800,true));QCOMPARE(session.clipRanges()[2],QVariant(QVariantList{4000.0,4800.0,true}));
        QVERIFY(session.setClipRange(track,480,960,false));
        const auto path=directory.filePath("ranges.json");QVERIFY(session.saveProject(path));
        {QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QVERIFY(file.readAll().contains("clip-ranges-v1"));}
        Session reopened;QVERIFY(reopened.loadProject(path));QCOMPARE(reopened.clipRanges()[2],QVariant(QVariantList{480.0,960.0,false}));
        // A file that became shorter opens whole, with a message.
        writeWav(ramp,1,48000,std::vector<qint16>(400,1000));
        Session shorter;QVERIFY(shorter.loadProject(path));QVERIFY(shorter.message().contains("range reset"));
        QCOMPARE(shorter.clipRanges()[2],QVariant(QVariantList{0.0,400.0,false})); // loop choice is kept
        // Re-importing replaces the source and plays it whole.
        QVERIFY(session.importAudio(track,ramp));QTRY_VERIFY(!session.importing());
        QCOMPARE(session.clipRanges()[2],QVariant(QVariantList{0.0,400.0,true}));
        session.undo();QCOMPARE(session.clipRanges()[2],QVariant(QVariantList{480.0,960.0,false}));
    }
    void qmlClipRange() {
        QTemporaryDir directory;const auto ramp=directory.filePath("ramp.wav");
        std::vector<qint16> samples(48000);for(std::size_t i=0;i<samples.size();++i)samples[i]=static_cast<qint16>(std::sin(static_cast<double>(i)/200)*20000);
        writeWav(ramp,1,48000,samples);
        Session session;QVERIFY(session.importAudio(session.trackIds()[0].toString(),ramp));QTRY_VERIFY(!session.importing());
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* root,const QString& name)->QQuickItem* {
            if(root->objectName()==name && root->isVisible())return root;
            for(auto* child:root->childItems())if(auto* match=findItem(child,name))return match;
            return nullptr;
        };
        QVERIFY(findItem(window->contentItem(),"waveform0"));QVERIFY(!findItem(window->contentItem(),"waveform1"));
        auto* start=findItem(window->contentItem(),"rangeStart0");QVERIFY(start);
        start->forceActiveFocus();QTest::keyClick(window,Qt::Key_Up);
        QCOMPARE(session.clipRanges()[0],QVariant(QVariantList{48.0,48000.0,true}));
        auto* end=findItem(window->contentItem(),"rangeEnd0");QVERIFY(end);
        end->forceActiveFocus();QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        for(const char c:QByteArray("250"))QTest::keyClick(window,c);
        QTest::keyClick(window,Qt::Key_Return);
        QTRY_COMPARE(session.clipRanges()[0],QVariant(QVariantList{48.0,12000.0,true}));
        auto* loop=findItem(window->contentItem(),"rangeLoop0");
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,loop->mapToScene(QPointF(loop->width()/2,loop->height()/2)).toPoint());
        QCOMPARE(session.clipRanges()[0],QVariant(QVariantList{48.0,12000.0,false}));
        QCOMPARE(findItem(window->contentItem(),"waveform0")->property("paintKey").toString().section(':',0,3),QString("48000:48:12000:false"));
        for(const auto* name:{"rangeLoop0","solo0","importAudio0","deleteTrack0"}) {
            auto* item=findItem(window->contentItem(),name);QVERIFY2(item,name);
            QVERIFY2(item->mapToItem(findItem(window->contentItem(),"clip1"),QPointF(item->width(),0)).x()<0,name); // stays left of track 2
        }
        QVERIFY(window->grabWindow().save("desktop-clip-range.png"));
        session.undo();QCOMPARE(session.clipRanges()[0],QVariant(QVariantList{48.0,12000.0,true}));
        QTRY_VERIFY(findItem(window->contentItem(),"rangeLoop0")->property("checked").toBool());
    }
    void qmlMixerInput() {
        Session session;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* root,const QString& name)->QQuickItem* {
            if(root->objectName()==name && root->isVisible())return root;
            for(auto* child:root->childItems())if(auto* match=findItem(child,name))return match;
            return nullptr;
        };
        auto click=[&](const char* name) {
            auto* item=findItem(window->contentItem(),QString::fromUtf8(name));QVERIFY(item);
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());
        };
        auto* pan=findItem(window->contentItem(),"pan1");QVERIFY(pan);
        QCOMPARE(pan->property("displayText").toString(),QString("C"));
        pan->forceActiveFocus();QTest::keyClick(window,Qt::Key_Down);
        QCOMPARE(session.pans(),(QVariantList{0,-5}));QCOMPARE(pan->property("displayText").toString(),QString("L5"));
        // Typed entry in L/R notation.
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        for(const char c:QByteArray("R35"))QTest::keyClick(window,c);
        QTest::keyClick(window,Qt::Key_Return);
        QTRY_COMPARE(session.pans(),(QVariantList{0,35}));
        click("mute0");QCOMPARE(session.mutes(),(QVariantList{true,false}));
        click("solo1");QCOMPARE(session.solos(),(QVariantList{false,true}));
        QVERIFY(findItem(window->contentItem(),"mute0")->property("checked").toBool());
        findItem(window->contentItem(),"solo0")->forceActiveFocus();QTest::keyClick(window,Qt::Key_Space);
        QCOMPARE(session.solos(),(QVariantList{true,true}));
        click("undoButton");QCOMPARE(session.solos(),(QVariantList{false,true}));
        QVERIFY(!findItem(window->contentItem(),"solo0")->property("checked").toBool());
        click("undoButton");click("undoButton");QCOMPARE(session.mutes(),(QVariantList{false,false}));QCOMPARE(session.solos(),(QVariantList{false,false}));
        click("redoButton");QCOMPARE(session.mutes(),(QVariantList{true,false}));
        session.play();session.tick();
        QVERIFY(!findItem(window->contentItem(),"pan1")->property("enabled").toBool());
        QVERIFY(!findItem(window->contentItem(),"mute0")->property("enabled").toBool());
        session.stop();session.tick();
        QVERIFY(window->grabWindow().save("desktop-track-mixer.png"));
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
        // Malformed legacy (v2) settings built from the saved pattern.
        const auto document=QJsonDocument::fromJson(encodeProjectSettings(readAuthoredSettings(good))).object();
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
        click("exportWavButton");QTRY_VERIFY(!session.exporting());QVERIFY(QFile::exists(directory.filePath("qml.wav")));QVERIFY(session.dirty());
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
