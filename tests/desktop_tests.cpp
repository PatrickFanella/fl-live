#include "session.hpp"
#include <QtTest>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QAccessible>
#include <QtEndian>
#include <cstring>
#include <tuple>
#include <QFileInfo>
#include <cmath>
#include <csignal>
#include <sys/resource.h>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <QTemporaryDir>
#include <QProcess>
#include <QScopeGuard>
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

// Independent WAV reader for export checks: header fields and decoded values.
struct DecodedWav { int format=0,channels=0,bits=0,rate=0,blockAlign=0,byteRate=0; std::vector<float> samples; qsizetype dataBytes=0; };
static DecodedWav readWav(const QString& path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("export missing");
    const auto b=file.readAll();DecodedWav w;
    if(b.size()<44 || b.left(4)!="RIFF" || b.mid(8,8)!="WAVEfmt " || b.mid(36,4)!="data")throw std::runtime_error("not a canonical WAV");
    w.format=qFromLittleEndian<quint16>(b.constData()+20);w.channels=qFromLittleEndian<quint16>(b.constData()+22);
    w.rate=static_cast<int>(qFromLittleEndian<quint32>(b.constData()+24));w.byteRate=static_cast<int>(qFromLittleEndian<quint32>(b.constData()+28));
    w.blockAlign=qFromLittleEndian<quint16>(b.constData()+32);w.bits=qFromLittleEndian<quint16>(b.constData()+34);
    w.dataBytes=static_cast<qsizetype>(qFromLittleEndian<quint32>(b.constData()+40));
    if(w.dataBytes!=b.size()-44 || qFromLittleEndian<quint32>(b.constData()+4)!=static_cast<quint32>(b.size()-8))throw std::runtime_error("size fields");
    const auto* d=reinterpret_cast<const unsigned char*>(b.constData()+44);
    for(qsizetype at=0;at<w.dataBytes;at+=w.bits/8) {
        if(w.format==3 && w.bits==32){float v;std::memcpy(&v,d+at,4);w.samples.push_back(v);}
        else if(w.bits==16)w.samples.push_back(static_cast<float>(static_cast<qint16>(d[at]|(d[at+1]<<8))));
        else if(w.bits==24){qint32 v=d[at]|(d[at+1]<<8)|(d[at+2]<<16);if(v&0x800000)v-=0x1000000;w.samples.push_back(static_cast<float>(v));}
    }
    return w;
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
        QVERIFY(missing.message().contains("Missing audio"));QCOMPARE(missing.trackSources()[2].toString(),QString("Missing: loop.wav"));
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
    void qmlD2Journey() {
        // #37 qualification workload, driven only through the QML: two audio
        // and two MIDI tracks, four scenes, mixing, Save, close, Open, Export.
        QTemporaryDir directory;
        const auto kick=directory.filePath("kick.wav"),pad=directory.filePath("pad.wav");
        std::vector<qint16> kickSamples(4800),padSamples(9600*2);
        for(std::size_t i=0;i<kickSamples.size();++i)kickSamples[i]=static_cast<qint16>(16000.0*std::exp(-static_cast<double>(i)/600)*std::sin(static_cast<double>(i)/30));
        for(std::size_t i=0;i<9600;++i){padSamples[2*i]=static_cast<qint16>(6000*std::sin(static_cast<double>(i)/50));padSamples[2*i+1]=static_cast<qint16>(6000*std::sin(static_cast<double>(i)/45));}
        writeWav(kick,1,48000,kickSamples);writeWav(pad,2,48000,padSamples);
        const auto projectPath=directory.filePath("journey.json"),exportPath=directory.filePath("journey.wav");
        std::vector<QStringList> sceneTracks;QVariantList trackIds;
        {
            Session session;
            QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
            engine.load(QUrl("qrc:/ui/Main.qml"));
            auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
            std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* root,const QString& name)->QQuickItem* {
                if(root->objectName()==name && root->isVisible())return root;
                for(auto* child:root->childItems())if(auto* match=findItem(child,name))return match;
                return nullptr;
            };
            auto show=[&](QQuickItem* item) {
                auto* scroll=findItem(window->contentItem(),"trackScroll");if(!scroll)return;
                auto* flick=scroll->property("contentItem").value<QQuickItem*>();if(!flick)return;
                const auto at=item->mapToItem(flick,QPointF(0,0));
                if(at.x()<0 || at.x()+item->width()>flick->width())
                    flick->setProperty("contentX",std::max(0.0,flick->property("contentX").toReal()+at.x()-20));
                QTest::qWait(20);
            };
            auto click=[&](const QString& name) {
                auto* item=findItem(window->contentItem(),name);QVERIFY2(item,qPrintable(name));show(item);
                QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());
            };
            auto focus=[&](const QString& name) {
                auto* item=findItem(window->contentItem(),name);QVERIFY2(item,qPrintable(name));show(item);item->forceActiveFocus();
            };
            auto type=[&](const QString& name,const QString& text,bool enter=true) {
                focus(name);QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);for(const QChar c:text)QTest::keyClick(window,c.toLatin1());
                if(enter)QTest::keyClick(window,Qt::Key_Return);
            };
            auto keys=[&](const QString& name,Qt::Key key,int times){focus(name);for(int i=0;i<times;++i)QTest::keyClick(window,key);};
            // Two audio tracks with imported WAVs, two MIDI tracks with patterns.
            type("audioPath0",kick,false);click("importAudio0");QTRY_VERIFY(!session.importing());
            click("addAudioTrack");QTRY_COMPARE(session.trackCount(),3);
            type("audioPath2",pad,false);click("importAudio2");QTRY_VERIFY(!session.importing());
            click("addMidiTrack");QTRY_COMPARE(session.trackCount(),4);
            QCOMPARE(session.trackKinds(),(QVariantList{"audio","midi","audio","midi"}));
            click("editPattern3");click("step0");click("stepEnabled");keys("selectedPitch",Qt::Key_Down,12);
            click("step2");click("stepEnabled");keys("selectedGate",Qt::Key_Up,25);
            click("editPattern1");click("step3");click("stepEnabled"); // rest on the template's fourth step
            // Mix: tempo, levels, pans, and a clip range on the first audio track.
            type("tempoSpin","128");QCOMPARE(session.tempo(),128);
            keys("level2",Qt::Key_Down,10);type("pan2","L40");type("pan3","R20");keys("level1",Qt::Key_Up,15);
            type("rangeEnd0","50");QCOMPARE(session.clipRanges()[0],QVariant(QVariantList{0.0,2400.0,true}));
            // Four scenes: template scene plus three arrangements.
            click("sceneIncluded2");click("sceneIncluded3");
            click("createScene");click("sceneIncluded2");click("sceneIncluded3");
            click("copyScene");click("sceneIncluded2");
            click("createScene");click("sceneIncluded0");
            QCOMPARE(session.sceneIds().size(),4);
            trackIds=session.trackIds();
            for(const auto& id:session.sceneIds()) {
                QVERIFY(session.selectScene(id.toString()));QStringList included;
                for(int t=0;t<4;++t)if(session.sceneIncluded()[t].toBool())included.append(trackIds[t].toString());
                sceneTracks.push_back(included);
            }
            QCOMPARE(sceneTracks[0].size(),4);QCOMPARE(sceneTracks[1].size(),2);QCOMPARE(sceneTracks[2].size(),1);QCOMPARE(sceneTracks[3].size(),1);
            // Launch scenes through the controls (silent preview clock).
            focus("sceneSelector");QTest::keyClick(window,Qt::Key_Home);
            QTRY_COMPARE(session.selectedSceneIndex(),0);
            findItem(window->contentItem(),"sceneTiming")->setProperty("currentIndex",0);
            click("launchScene");click("playButton");QTRY_COMPARE(session.sceneState(),QString("Active"));
            QCOMPARE(session.activeClips(),(QVariantList{0,0,0,0}));
            focus("sceneSelector");QTest::keyClick(window,Qt::Key_Down);QTRY_COMPARE(session.selectedSceneIndex(),1);
            click("launchScene");QTRY_COMPARE(session.sceneState(),QString("Active"));
            QCOMPARE(session.activeClips(),(QVariantList{-1,-1,0,0}));
            click("stopButton");QTRY_VERIFY(!session.running());QTRY_VERIFY(session.canEdit());
            QVERIFY(window->grabWindow().save("desktop-d2-journey.png"));
            type("projectFile",projectPath,false);click("saveProjectButton");QVERIFY2(!session.dirty(),qPrintable(session.message()));
            QVERIFY(window->close());
        }
        // Reopen in a fresh session and window.
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
            auto* flick=findItem(window->contentItem(),"trackScroll")->property("contentItem").value<QQuickItem*>();
            const auto at=item->mapToItem(flick,QPointF(0,0));
            if(item->isAncestorOf(flick) || !flick->isAncestorOf(item)){} else if(at.x()<0 || at.x()+item->width()>flick->width())
                {flick->setProperty("contentX",std::max(0.0,flick->property("contentX").toReal()+at.x()-20));QTest::qWait(20);}
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());
        };
        auto type=[&](const QString& name,const QString& text) {
            auto* item=findItem(window->contentItem(),name);QVERIFY2(item,qPrintable(name));item->forceActiveFocus();
            QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);for(const QChar c:text)QTest::keyClick(window,c.toLatin1());
        };
        type("projectFile",projectPath);click("openProjectButton");
        QCOMPARE(session.trackIds(),trackIds);QCOMPARE(session.tempo(),128);
        QCOMPARE(session.trackKinds(),(QVariantList{"audio","midi","audio","midi"}));
        QCOMPARE(session.trackSources()[0].toString(),QString("kick.wav · 0.10 s · mono"));
        QCOMPARE(session.trackSources()[2].toString(),QString("pad.wav · 0.20 s · stereo"));
        QCOMPARE(session.levels(),(QVariantList{70,40,60,70}));QCOMPARE(session.pans(),(QVariantList{0,0,-40,20}));
        QCOMPARE(session.clipRanges()[0],QVariant(QVariantList{0.0,2400.0,true}));
        std::size_t scene=0;
        for(const auto& id:session.sceneIds()) {
            QVERIFY(session.selectScene(id.toString()));QStringList included;
            for(int t=0;t<4;++t)if(session.sceneIncluded()[t].toBool())included.append(trackIds[t].toString());
            QCOMPARE(included,sceneTracks[scene++]);
        }
        click("editPattern3");
        QCOMPARE(session.stepEnabled()[0].toBool(),true);QCOMPARE(session.pitches()[0].toInt(),48);
        QCOMPARE(session.stepEnabled()[2].toBool(),true);QCOMPARE(session.gates()[2].toInt(),100);
        click("editPattern1");QCOMPARE(session.stepEnabled()[3].toBool(),false);
        // Export through the UI and decode independently.
        type("exportFile",exportPath);findItem(window->contentItem(),"exportBars")->setProperty("value",2);
        click("exportWavButton");QTRY_VERIFY(!session.exporting());QVERIFY2(QFile::exists(exportPath),qPrintable(session.message()));
        const auto frames=readStereo(exportPath);QCOMPARE(frames.size(),static_cast<std::size_t>(exportFrames(128,2)));
        const auto n=frames.size();
        std::vector<float> kickSource(2400);for(std::size_t i=0;i<2400;++i)kickSource[i]=static_cast<float>(kickSamples[i])/32768;
        fl_live::AudioClip padClip{{},true,2};for(auto v:padSamples)padClip.samples.push_back(static_cast<float>(v)/32768);
        StepPattern template1;template1.steps[3].enabled=false;
        StepPattern bass;for(auto& step:bass.steps)step.enabled=false;bass.steps[0]={true,48,70,75};bass.steps[2]={true,67,70,100};
        const auto a=renderClip(fl_live::AudioClip{kickSource,true},128,n),b=renderClip(patternClip(template1,128),128,n);
        const auto d=renderClip(patternClip(bass,128),128,n);
        const auto pcm=[](float v){return static_cast<int>(std::lround(std::clamp(v,-1.0F,1.0F)*32767.0F));};
        int worst=0;
        for(std::size_t i=0;i<n;++i) {
            const float padLeft=padClip.samples[2*(i%9600)],padRight=padClip.samples[2*(i%9600)+1];
            // Pad: level 60, pan L40 (right x0.6). Bass: level 70, pan R20 (left x0.8).
            const float left=0.7F*a[i]+0.4F*b[i]+0.6F*padLeft+0.7F*0.8F*d[i];
            const float right=0.7F*a[i]+0.4F*b[i]+0.6F*0.6F*padRight+0.7F*d[i];
            worst=std::max({worst,std::abs(frames[i][0]-pcm(left)),std::abs(frames[i][1]-pcm(right))});
        }
        QVERIFY2(worst<=1,qPrintable(QString("export differs from independent renders by %1 LSB").arg(worst)));
    }
    void assetRelocationRelinkCollect() {
        QTemporaryDir root;
        std::vector<qint16> ramp(4800);for(std::size_t i=0;i<ramp.size();++i)ramp[i]=static_cast<qint16>((i%480)*64);
        const auto bytesOf=[](const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();};
        // A project folder with its audio inside moves as a whole.
        const auto song=root.filePath("song");QVERIFY(QDir().mkpath(song+"/audio"));
        writeWav(song+"/audio/loop.wav",1,48000,ramp);
        {
            Session session;QVERIFY(session.addTrack("audio"));const auto track=session.trackIds()[2].toString();
            QVERIFY(session.importAudio(track,song+"/audio/loop.wav"));QTRY_VERIFY(!session.importing());
            QVERIFY(session.setClipRange(track,480,960,false));QVERIFY(session.saveProject(song+"/song.json"));
            const auto saved=bytesOf(song+"/song.json");
            QVERIFY(saved.contains("asset-relative-v1"));QVERIFY(saved.contains("\"relative\": \"audio/loop.wav\""));
        }
        QVERIFY(QDir().rename(song,root.filePath("moved")));
        const auto moved=root.filePath("moved");
        {
            Session session;QVERIFY(session.loadProject(moved+"/song.json"));
            QVERIFY2(session.message().contains("Found moved audio"),qPrintable(session.message()));QVERIFY(session.dirty());
            QCOMPARE(session.trackSources()[2].toString(),QString("loop.wav · 0.10 s · mono"));
            QCOMPARE(session.clipRanges()[2],QVariant(QVariantList{480.0,960.0,false}));
            QVERIFY(session.saveProject(moved+"/song.json"));QVERIFY(!session.dirty());
            QVERIFY(bytesOf(moved+"/song.json").contains(QFileInfo(moved+"/audio/loop.wav").absoluteFilePath().toUtf8()));
        }
        // The saved absolute path wins whenever it exists: copying a project
        // folder does not silently switch it to the copy's audio.
        const auto copy=root.filePath("copy");QVERIFY(QDir().mkpath(copy+"/audio"));
        QVERIFY(QFile::copy(moved+"/song.json",copy+"/song.json"));
        writeWav(copy+"/audio/loop.wav",1,48000,std::vector<qint16>(4800,500));
        {
            Session session;QVERIFY(session.loadProject(copy+"/song.json"));QVERIFY(!session.dirty());
            QVERIFY(!session.message().contains("moved"));
            QCOMPARE(session.waveform(2,1)[0].toList()[1].toFloat(),479.0F*64/32768); // the original ramp, not the copy's constant
        }
        // Missing audio: relink keeps the range, refuses a file too short for it.
        QVERIFY(QFile::remove(moved+"/audio/loop.wav"));
        Session session;QVERIFY(session.loadProject(moved+"/song.json"));
        QVERIFY(session.message().contains("Missing audio"));QCOMPARE(session.trackSources()[2].toString(),QString("Missing: loop.wav"));
        const auto track=session.trackIds()[2].toString();
        const auto shortFile=root.filePath("short.wav");writeWav(shortFile,1,48000,std::vector<qint16>(900,1));
        QVERIFY(session.relinkAudio(track,shortFile));QTRY_VERIFY(!session.importing());
        QVERIFY(session.message().startsWith("Relink refused"));QCOMPARE(session.trackSources()[2].toString(),QString("Missing: loop.wav"));
        QVERIFY(!session.relinkAudio(session.trackIds()[1].toString(),shortFile)); // MIDI track
        QVERIFY(!session.relinkAudio(session.trackIds()[0].toString(),shortFile)); // built-in pulse
        const auto replacement=root.filePath("found/loop-found.wav");QVERIFY(QDir().mkpath(root.filePath("found")));writeWav(replacement,1,48000,ramp);
        QVERIFY(session.relinkAudio(track,replacement));QTRY_VERIFY(!session.importing());
        QCOMPARE(session.trackSources()[2].toString(),QString("loop-found.wav · 0.10 s · mono"));
        QCOMPARE(session.clipRanges()[2],QVariant(QVariantList{480.0,960.0,false}));QCOMPARE(session.undoText(),QString("Undo track 3 audio relink"));
        session.undo();QCOMPARE(session.trackSources()[2].toString(),QString("Missing: loop.wav"));session.redo();
        // Collect copies outside audio beside the project, never overwriting.
        const auto existing=moved+"/song-assets/loop-found.wav";QVERIFY(QDir().mkpath(moved+"/song-assets"));
        {QFile f(existing);QVERIFY(f.open(QIODevice::WriteOnly));f.write("someone else's file");}
        const auto sourceBytes=bytesOf(replacement);
        QVERIFY(session.collectAssets());QVERIFY2(session.message().contains("1 copied"),qPrintable(session.message()));
        QCOMPARE(bytesOf(existing),QByteArray("someone else's file"));QCOMPARE(bytesOf(replacement),sourceBytes);
        QCOMPARE(bytesOf(moved+"/song-assets/loop-found-2.wav"),sourceBytes);
        QCOMPARE(session.undoText(),QString("Undo audio collection"));
        QVERIFY(session.saveProject(moved+"/song.json"));
        QVERIFY(bytesOf(moved+"/song.json").contains("\"relative\": \"song-assets/loop-found-2.wav\""));
        QVERIFY(session.collectAssets());QVERIFY(session.message().contains("already inside"));
        session.undo();QVERIFY(session.dirty()); // back to the outside path
        QVERIFY(session.collectAssets());QVERIFY2(session.message().contains("0 copied, 1 already there"),qPrintable(session.message()));
        Session unsaved;QVERIFY(!unsaved.collectAssets());QVERIFY(unsaved.message().contains("Save the project first"));
    }
    void qmlRelinkMissingAudio() {
        QTemporaryDir root;const auto audio=root.filePath("a.wav"),project=root.filePath("p.json");
        std::vector<qint16> tone(4800);for(std::size_t i=0;i<tone.size();++i)tone[i]=static_cast<qint16>(i%300*50);
        writeWav(audio,1,48000,tone);
        {Session s;QVERIFY(s.importAudio(s.trackIds()[0].toString(),audio));QTRY_VERIFY(!s.importing());QVERIFY(s.saveProject(project));}
        QVERIFY(QFile::remove(audio));
        Session session;QVERIFY(session.loadProject(project));QCOMPARE(session.trackSources()[0].toString(),QString("Missing: a.wav"));
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* r,const QString& name)->QQuickItem* {
            if(r->objectName()==name && r->isVisible())return r;
            for(auto* c:r->childItems())if(auto* m=findItem(c,name))return m;
            return nullptr;
        };
        QCOMPARE(findItem(window->contentItem(),"trackSource0")->property("text").toString(),QString("Missing: a.wav"));
        const auto moved=root.filePath("moved.wav");writeWav(moved,1,48000,tone);
        auto* path=findItem(window->contentItem(),"audioPath0");QVERIFY(path);path->forceActiveFocus();
        for(const QChar c:moved)QTest::keyClick(window,c.toLatin1());
        auto* relink=findItem(window->contentItem(),"relinkAudio0");QVERIFY(relink);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,relink->mapToScene(QPointF(relink->width()/2,relink->height()/2)).toPoint());
        QTRY_COMPARE(session.trackSources()[0].toString(),QString("moved.wav · 0.10 s · mono"));
        QVERIFY(findItem(window->contentItem(),"waveform0"));QVERIFY(session.dirty());
        QVERIFY(!findItem(window->contentItem(),"relinkAudio1")); // MIDI track: no relink
    }
    void autosaveAndCrashRecovery() {
        QTemporaryDir recovery,work;
        qputenv("FL_LIVE_RECOVERY_DIR",recovery.path().toUtf8());
        const auto cleanup=qScopeGuard([]{qunsetenv("FL_LIVE_RECOVERY_DIR");});
        const auto bytesOf=[](const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();};
        // A real crash: the probe saves, edits, autosaves and aborts.
        const auto probe=qEnvironmentVariable("FL_LIVE_SAVE_PROBE");QVERIFY2(!probe.isEmpty(),"FL_LIVE_SAVE_PROBE not set");
        QProcess crashed;crashed.start(probe,{"crash",work.path()});
        QVERIFY(crashed.waitForFinished(20000));QCOMPARE(crashed.exitStatus(),QProcess::CrashExit);
        const auto snapshotPath=QString::fromUtf8(crashed.readAllStandardOutput()).trimmed();
        QVERIFY(QFile::exists(snapshotPath));QVERIFY(snapshotPath.startsWith(recovery.path()));
        const auto project=work.filePath("crashed.json");const auto manual=bytesOf(project);
        QCOMPARE(readAuthoredProject(project).content.tempo,120); // autosave never wrote the project file
        // Invalid snapshots are listed with a reason and left alone.
        const auto garbage=QDir(recovery.path()).filePath("garbage.fl-live-recovery.json");
        {QFile f(garbage);QVERIFY(f.open(QIODevice::WriteOnly));f.write("{\"format\":\"fl-live-recovery\"");}
        const auto truncated=QDir(recovery.path()).filePath("truncated.fl-live-recovery.json");
        {QFile f(truncated);QVERIFY(f.open(QIODevice::WriteOnly));f.write(bytesOf(snapshotPath).left(200));}
        Session session;
        const auto candidates=session.recoveryCandidates();QCOMPARE(candidates.size(),3);
        QVariantMap valid;int invalid=0;
        for(const auto& c:candidates){const auto m=c.toMap();if(m["valid"].toBool())valid=m;else{++invalid;QVERIFY(!m["reason"].toString().isEmpty());}}
        QCOMPARE(invalid,2);QCOMPARE(valid["file"].toString(),snapshotPath);QCOMPARE(valid["project"].toString(),project);QCOMPARE(valid["tracks"].toInt(),3);
        QVERIFY(!session.restoreRecovery(garbage));QVERIFY(QFile::exists(garbage));
        // Restore: the crashed edits come back unsaved; the manual save is unchanged until Save.
        QVERIFY(session.restoreRecovery(snapshotPath));
        QCOMPARE(session.tempo(),141);QCOMPARE(session.trackCount(),3);QVERIFY(session.dirty());
        QCOMPARE(session.projectPath(),project);QCOMPARE(bytesOf(project),manual);
        QVERIFY(!QFile::exists(snapshotPath));QVERIFY(QFile::exists(session.autosaveFile())); // replaced by this session's own snapshot
        QCOMPARE(session.recoveryCandidates().size(),2); // only the invalid ones remain listed
        QVERIFY(session.saveProject(project));QVERIFY(!session.dirty());QCOMPARE(readAuthoredProject(project).content.tempo,141);
        QVERIFY(!QFile::exists(session.autosaveFile()));
        // Restoring is refused over unsaved edits; Discard removes one file only.
        session.setTempo(150);QVERIFY(!session.restoreRecovery(garbage));
        QVERIFY(session.discardRecovery(garbage));QVERIFY(!QFile::exists(garbage));QVERIFY(QFile::exists(truncated));
        QVERIFY(!session.discardRecovery(work.filePath("crashed.json")));QVERIFY(QFile::exists(project)); // not a listed snapshot
        // Autosave only while dirty, never to the project; a running session's
        // snapshot is not offered to another session.
        QVERIFY(session.autosaveNow());const auto own=session.autosaveFile();QVERIFY(QFile::exists(own));
        Session other;
        for(const auto& c:other.recoveryCandidates())QVERIFY(c.toMap()["file"].toString()!=own);
        session.undo();QVERIFY(!session.dirty());QVERIFY(!session.autosaveNow());
        // A normal shutdown removes its snapshot.
        QString removed;
        {Session temporary;temporary.setTempo(99);QVERIFY(temporary.autosaveNow());removed=temporary.autosaveFile();QVERIFY(QFile::exists(removed));}
        QVERIFY(!QFile::exists(removed));
        QDir dir(recovery.path());QCOMPARE(dir.entryList({"*.fl-live-recovery.json"}).size(),2); // truncated + the live session's
        // The banner offers Restore for a valid snapshot through the UI.
        QVERIFY(QDir().mkpath(work.filePath("second")));
        QProcess again;again.start(probe,{"crash",work.filePath("second")});
        QVERIFY(again.waitForFinished(20000));QCOMPARE(again.exitStatus(),QProcess::CrashExit);
        Session fresh;
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&fresh);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* r,const QString& name)->QQuickItem* {
            if(r->objectName()==name && r->isVisible())return r;
            for(auto* c:r->childItems())if(auto* m=findItem(c,name))return m;
            return nullptr;
        };
        int validIndex=-1;
        for(int i=0;i<fresh.recoveryCandidates().size();++i)if(fresh.recoveryCandidates()[i].toMap()["valid"].toBool())validIndex=i;
        QVERIFY(validIndex>=0);
        QVERIFY(window->grabWindow().save("desktop-recovery.png"));
        auto* restore=findItem(window->contentItem(),QString("restoreRecovery%1").arg(validIndex));QVERIFY(restore);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,restore->mapToScene(QPointF(restore->width()/2,restore->height()/2)).toPoint());
        QTRY_COMPARE(fresh.tempo(),141);QVERIFY(fresh.dirty());
        QCOMPARE(fresh.projectPath(),work.filePath("second/crashed.json"));
    }
    void exportRenderPolicy() {
        QTemporaryDir directory;
        Session session;session.setTempo(133); // unsaved edit: the export must use it
        QVERIFY(session.dirty());
        const auto bar=exportFrames(133,1),twoBars=exportFrames(133,2);
        const auto pulse=renderClip(pulseClip(133),133,twoBars),steps=renderClip(patternClip(StepPattern{},133),133,twoBars);
        // Independent expectation: centered template, levels 70 and 25.
        const auto mix=[&](std::size_t frame){return 0.7F*pulse[frame]+0.25F*steps[frame];};
        const auto run=[&](const QString& name,int start,int bars,int tail,int channels,const char* format){
            QVERIFY(session.exportAudio(directory.filePath(name),start,bars,tail,channels,format));QTRY_VERIFY(!session.exporting());
            QVERIFY2(QFile::exists(directory.filePath(name)),qPrintable(session.message()));
        };
        struct Case{const char* name;int channels;const char* format;int code,bits;float scale;};
        for(const Case c:{Case{"s16.wav",2,"pcm16",1,16,32767.0F},Case{"m24.wav",1,"pcm24",1,24,8388607.0F},Case{"s32.wav",2,"float32",3,32,0.0F}}) {
            run(c.name,2,1,100,c.channels,c.format);
            const auto w=readWav(directory.filePath(c.name));
            QCOMPARE(w.format,c.code);QCOMPARE(w.bits,c.bits);QCOMPARE(w.channels,c.channels);QCOMPARE(w.rate,48000);
            QCOMPARE(w.blockAlign,c.channels*c.bits/8);QCOMPARE(w.byteRate,48000*c.channels*c.bits/8);
            const auto frames=static_cast<std::size_t>(twoBars-bar)+4800; // bar 2 plus a 100 ms tail
            QCOMPARE(w.samples.size(),frames*static_cast<std::size_t>(c.channels));
            float worst=0;
            for(std::size_t f=0;f<frames;++f)for(int ch=0;ch<c.channels;++ch) {
                const float value=f<twoBars-bar ? mix(static_cast<std::size_t>(bar)+f) : 0.0F; // tail: every track stopped
                const float expected=c.scale>0 ? static_cast<float>(std::lround(std::clamp(value,-1.0F,1.0F)*c.scale)) : value;
                worst=std::max(worst,std::abs(w.samples[f*static_cast<std::size_t>(c.channels)+static_cast<std::size_t>(ch)]-expected));
            }
            QVERIFY2(worst<=(c.scale>0 ? 1.0F : 1e-7F),qPrintable(QString("%1 differs by %2").arg(c.name).arg(worst)));
        }
        // Mono is the (L + R) / 2 downmix: with hard pan the level halves.
        QVERIFY(session.setPan(session.trackIds()[0].toString(),-100));QVERIFY(session.setMute(session.trackIds()[1].toString(),true));
        run("pan-mono.wav",1,1,0,1,"float32");run("pan-stereo.wav",1,1,0,2,"float32");
        const auto mono=readWav(directory.filePath("pan-mono.wav")),stereo=readWav(directory.filePath("pan-stereo.wav"));
        for(std::size_t f=0;f<static_cast<std::size_t>(bar);f+=101) {
            QCOMPARE(stereo.samples[2*f+1],0.0F);QCOMPARE(mono.samples[f],(stereo.samples[2*f]+stereo.samples[2*f+1])/2);
        }
        // Refused options leave no file and no partial file.
        const auto refused=directory.filePath("refused.wav");
        for(const auto& bad:std::vector<std::tuple<int,int,int,int,QString>>{{0,1,0,2,"pcm16"},{257,1,0,2,"pcm16"},{1,0,0,2,"pcm16"},{1,65,0,2,"pcm16"},
                                                                          {1,1,-1,2,"pcm16"},{1,1,10001,2,"pcm16"},{1,1,0,3,"pcm16"},{1,1,0,2,"mp3"}}) {
            QVERIFY(!session.exportAudio(refused,std::get<0>(bad),std::get<1>(bad),std::get<2>(bad),std::get<3>(bad),std::get<4>(bad)));
            QVERIFY(!session.exporting());
        }
        QVERIFY(!QFile::exists(refused));QVERIFY(!QFile::exists(refused+".part"));
        // The longest range stays within the WAV size limit and starts on its bar.
        session.setTempo(300);QCOMPARE(session.tempo(),300);run("late.wav",256,1,0,1,"pcm16");
        QCOMPARE(readWav(directory.filePath("late.wav")).samples.size(),static_cast<std::size_t>(exportFrames(300,256)-exportFrames(300,255)));
        // Actual UI: options chosen with keyboard and the combo boxes.
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* r,const QString& name)->QQuickItem* {
            if(r->objectName()==name && r->isVisible())return r;
            for(auto* c:r->childItems())if(auto* m=findItem(c,name))return m;
            return nullptr;
        };
        findItem(window->contentItem(),"exportFile")->setProperty("text",directory.filePath("ui.wav"));
        findItem(window->contentItem(),"exportStart")->forceActiveFocus();QTest::keyClick(window,Qt::Key_Up);
        findItem(window->contentItem(),"exportTail")->forceActiveFocus();QTest::keyClick(window,Qt::Key_Up);
        findItem(window->contentItem(),"exportBars")->forceActiveFocus();for(int i=0;i<3;++i)QTest::keyClick(window,Qt::Key_Down);
        findItem(window->contentItem(),"exportChannels")->setProperty("currentIndex",1);
        findItem(window->contentItem(),"exportFormat")->setProperty("currentIndex",1);
        auto* button=findItem(window->contentItem(),"exportWavButton");
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,button->mapToScene(QPointF(button->width()/2,button->height()/2)).toPoint());
        QTRY_VERIFY(!session.exporting());
        const auto ui=readWav(directory.filePath("ui.wav"));
        QCOMPARE(ui.channels,1);QCOMPARE(ui.bits,24);
        QCOMPARE(ui.samples.size(),static_cast<std::size_t>(exportFrames(300,2)-exportFrames(300,1))+4800); // bar 2, 1 bar, 100 ms
    }
    void bundledExamplesOpen() {
        // Every shipped example opens cleanly (no missing audio, no
        // relocation), launches each scene and exports audible stereo.
        const QDir examples(qEnvironmentVariable("FL_LIVE_EXAMPLES_DIR"));
        QVERIFY2(examples.exists(),"FL_LIVE_EXAMPLES_DIR not set");
        const auto files=examples.entryList({"*.json"},QDir::Files);QVERIFY(files.size()>=2);
        QTemporaryDir out;
        for(const auto& name:files) {
            Session session;QVERIFY2(session.loadProject(examples.filePath(name)),qPrintable(name+": "+session.message()));
            QCOMPARE(session.message(),QString("Project opened. Launch clips, then press Play."));QVERIFY(!session.dirty());
            for(const auto& source:session.trackSources())QVERIFY(!source.toString().startsWith("Missing"));
            session.play();
            for(const auto& scene:session.sceneIds()) {
                QVERIFY(session.selectScene(scene.toString()));QVERIFY(session.launchScene(scene.toString(),"immediate"));
                QTRY_COMPARE(session.sceneState(),QString("Active"));
            }
            session.stop();QTRY_VERIFY(session.canEdit());
            QVERIFY(session.exportWav(out.filePath(name+".wav"),2));QTRY_VERIFY(!session.exporting());
            const auto frames=readStereo(out.filePath(name+".wav"));
            QVERIFY2(std::any_of(frames.begin(),frames.end(),[](const auto& f){return f[0]!=0 || f[1]!=0;}),qPrintable(name+" exported silence"));
            QVERIFY(!session.dirty());
        }
    }
    void keyboardContractAndFocus() {
        QTemporaryDir directory;
        Session session;
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* r,const QString& name)->QQuickItem* {
            if(r->objectName()==name && r->isVisible())return r;
            for(auto* c:r->childItems())if(auto* m=findItem(c,name))return m;
            return nullptr;
        };
        // Shortcut table: every sequence is declared once.
        QStringList sequences;
        for(auto* shortcut:window->findChildren<QObject*>()) if(QString(shortcut->metaObject()->className())=="QQuickShortcut") {
            QStringList own;
            for(const auto& v:shortcut->property("sequences").toList()) {
                // Standard keys expand to every platform binding they stand for.
                if(v.typeId()==QMetaType::Int)for(const auto& k:QKeySequence::keyBindings(static_cast<QKeySequence::StandardKey>(v.toInt())))own.append(k.toString());
                else own.append(v.toString());
            }
            if(own.isEmpty())own.append(QKeySequence(shortcut->property("sequence").toString()).toString());
            own.removeDuplicates();sequences+=own;
        }
        auto unique=sequences;unique.removeDuplicates();
        QVERIFY2(unique.size()==sequences.size() && sequences.size()>=9,qPrintable(sequences.join(" | ")));
        // Space toggles transport when no control would consume it.
        window->contentItem()->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Space);session.tick();QTRY_VERIFY(session.running());
        QTest::keyClick(window,Qt::Key_Space);session.tick();QTRY_VERIFY(!session.running());
        // In a text field, Space and Ctrl+Z are text editing, not transport or history.
        session.setTempo(121);
        auto* path=findItem(window->contentItem(),"projectFile");path->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_End);QTest::keyClick(window,Qt::Key_Space);session.tick();
        QVERIFY(!session.running());QVERIFY(path->property("text").toString().endsWith(" "));
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier);QCOMPARE(session.tempo(),121);
        // On a focused button, Space presses that button instead.
        auto* stopTrack=findItem(window->contentItem(),"stopTrack0");stopTrack->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Space);session.tick();QVERIFY(!session.running());
        // Ctrl+S saves to the path field; Ctrl+Shift+A/M add tracks.
        path->setProperty("text",directory.filePath("keys.json"));window->contentItem()->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_S,Qt::ControlModifier);QVERIFY(QFile::exists(directory.filePath("keys.json")));QVERIFY(!session.dirty());
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier|Qt::ShiftModifier);QTRY_COMPARE(session.trackCount(),3);
        QTest::keyClick(window,Qt::Key_M,Qt::ControlModifier|Qt::ShiftModifier);QTRY_COMPARE(session.trackCount(),4);
        QCOMPARE(session.trackKinds(),(QVariantList{"audio","midi","audio","midi"}));
        // Ctrl+E exports with the row's settings.
        findItem(window->contentItem(),"exportFile")->setProperty("text",directory.filePath("keys.wav"));
        window->contentItem()->forceActiveFocus();QTest::keyClick(window,Qt::Key_E,Qt::ControlModifier);
        QTRY_VERIFY(!session.exporting());QVERIFY(QFile::exists(directory.filePath("keys.wav")));
        // Keyboard deletion keeps focus on the next track's Delete button,
        // then on the previous one, then on Add audio track.
        auto* del=findItem(window->contentItem(),"deleteTrack1");del->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Space);QTRY_COMPARE(session.trackCount(),3);
        QTRY_COMPARE(window->activeFocusItem()->objectName(),QString("deleteTrack1"));
        QCOMPARE(session.trackKinds(),(QVariantList{"audio","audio","midi"}));
        findItem(window->contentItem(),"deleteTrack2")->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_Space);QTRY_COMPARE(session.trackCount(),2);
        QTRY_COMPARE(window->activeFocusItem()->objectName(),QString("deleteTrack1"));
        QTest::keyClick(window,Qt::Key_Space);QTRY_COMPARE(session.trackCount(),1);
        QTest::keyClick(window,Qt::Key_Space);QTRY_COMPARE(session.trackCount(),0);
        QTRY_COMPARE(window->activeFocusItem()->objectName(),QString("addAudioTrack"));
        // Undo from the keyboard restores tracks; focus stays on a visible control.
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier);QTRY_COMPARE(session.trackCount(),1);
        QVERIFY(window->activeFocusItem() && window->activeFocusItem()->isVisible());
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);QTRY_COMPARE(session.trackCount(),0);
        // Shortcuts are inactive while a dialog is open.
        session.undo();QCOMPARE(session.trackCount(),1);
        findItem(window->contentItem(),"renameTrack0")->forceActiveFocus();QTest::keyClick(window,Qt::Key_Space);
        QTRY_VERIFY(window->findChild<QObject*>("renameDialog")->property("visible").toBool());
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier|Qt::ShiftModifier);QCOMPARE(session.trackCount(),1);
        QTest::keyClick(window,Qt::Key_Escape);QTRY_VERIFY(!window->findChild<QObject*>("renameDialog")->property("visible").toBool());
        QTRY_COMPARE(window->activeFocusItem()->objectName(),QString("renameTrack0")); // focus returns to the opener
    }
    void accessibleNames() {
        // Every visible interactive control has an accessible name, and no
        // two controls share one, so a screen reader can tell them apart.
        Session session;QVERIFY(session.addTrack("audio"));QVERIFY(session.addTrack("midi"));
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        QAccessible::setActive(true);
        QStringList names,unnamed;int controls=0;
        std::function<void(QQuickItem*)> walk=[&](QQuickItem* item) {
            if(!item->isVisible())return;
            const QString type=item->metaObject()->className();
            // A ComboBox's own text field is part of the combo, which is named.
            const bool insideCombo=item->parentItem() && item->parentItem()->inherits("QQuickComboBox");
            const bool control=!insideCombo && (item->inherits("QQuickAbstractButton") || item->inherits("QQuickSpinBox") ||
                               item->inherits("QQuickComboBox") || item->inherits("QQuickTextField") || item->inherits("QQuickProgressBar"));
            if(control) {
                ++controls;
                auto* iface=QAccessible::queryAccessibleInterface(item);
                const auto name=iface ? iface->text(QAccessible::Name) : QString();
                if(name.trimmed().isEmpty())unnamed.append(type+"/"+item->objectName());else names.append(name);
                if(iface)QVERIFY2(iface->role()!=QAccessible::NoRole,qPrintable(name));
            }
            for(auto* child:item->childItems())walk(child);
        };
        walk(window->contentItem());
        QVERIFY(controls>60);
        QVERIFY2(unnamed.isEmpty(),qPrintable("unnamed: "+unnamed.join(", ")));
        QStringList duplicates;for(const auto& n:names)if(names.count(n)>1 && !duplicates.contains(n))duplicates.append(n);
        QVERIFY2(duplicates.isEmpty(),qPrintable("duplicate names: "+duplicates.join(", ")));
        // State is exposed too: a checked Mute reports the checked state.
        QVERIFY(session.setMute(session.trackIds()[0].toString(),true));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* r,const QString& name)->QQuickItem* {
            if(r->objectName()==name && r->isVisible())return r;
            for(auto* c:r->childItems())if(auto* m=findItem(c,name))return m;
            return nullptr;
        };
        QTRY_VERIFY(QAccessible::queryAccessibleInterface(findItem(window->contentItem(),"mute0"))->state().checked);
        QAccessible::setActive(false);
    }
    void generalDirtyAndHistoryBoundaries() {
        QTemporaryDir directory;const auto path=directory.filePath("bounds.json");
        Session session;QVERIFY(session.addTrack("audio"));QVERIFY(session.addTrack("midi"));
        QVERIFY(session.saveProject(path));QVERIFY(!session.dirty());
        // Selection, pattern choice, scene choice and export are not document edits.
        const auto undo=session.undoText();
        QVERIFY(session.editPattern(session.trackIds()[3].toString()));session.selectStep(2);
        QVERIFY(session.createScene());session.undo(); // structure edit then back
        QVERIFY(!session.dirty());QCOMPARE(session.undoText(),undo);
        QVERIFY(session.selectScene(session.sceneIds()[0].toString()));
        QVERIFY(session.exportWav(directory.filePath("x.wav"),1));QTRY_VERIFY(!session.exporting());
        QVERIFY(!session.dirty());QCOMPARE(session.undoText(),undo);
        // Returning to saved content clears dirty across structure edits.
        const auto ids=session.trackIds();
        QVERIFY(session.deleteTrack(ids[2].toString()));QVERIFY(session.dirty());
        QVERIFY(session.duplicateTrack(ids[0].toString()));session.undo();session.undo();QVERIFY(!session.dirty());
        QCOMPARE(session.trackIds(),ids);
        // Failed operations add no history and keep the document.
        QVERIFY(!session.deleteTrack("999"));QVERIFY(!session.duplicateTrack("0"));QVERIFY(!session.addTrack("bus"));
        QVERIFY(!session.setClipRange(ids[1].toString(),0,10,true));QVERIFY(!session.relinkAudio(ids[1].toString(),"/nope.wav"));
        QCOMPARE(session.undoText(),undo);QVERIFY(!session.dirty());
        // History stays bounded at 128 across mixed structure and content edits.
        for(int n=0;n<70;++n){QVERIFY(session.addTrack(n%2 ? "midi" : "audio"));QVERIFY(session.deleteTrack(session.trackIds().last().toString()));}
        int undos=0;while(session.canUndo() && undos<200){session.undo();++undos;}
        QCOMPARE(undos,128);QVERIFY(!session.canUndo());QVERIFY(!session.dirty()); // add/delete pairs: equal content is clean
        int redos=0;while(session.canRedo() && redos<200){session.redo();++redos;}
        QCOMPARE(redos,128);QCOMPARE(session.trackIds(),ids);QVERIFY(!session.dirty());
        // New is refused while dirty, then resets path, history and checkpoint.
        session.setTempo(99);QVERIFY(!session.newProject());QVERIFY(session.message().contains("Save your changes"));QCOMPARE(session.trackCount(),4);
        session.undo();QVERIFY(session.newProject());
        QCOMPARE(session.trackCount(),2);QVERIFY(!session.dirty());QVERIFY(!session.canUndo());QCOMPARE(session.tempo(),120);
        QVERIFY(session.projectPath()!=path);
        // The new project cannot silently overwrite the file it replaced.
        QVERIFY(!session.saveProject(path));QVERIFY(session.message().contains("already exists"));
        QCOMPARE(readAuthoredProject(path).identities.tracks.size(),4u);
    }
    void automationIdentityStability() {
        // Lanes are keyed by stable track IDs through rename, duplicate,
        // delete, Undo and Save/Open.
        QTemporaryDir directory;const auto path=directory.filePath("auto.json");
        auto project=defaultProject();
        const auto pulse=project.identities.tracks[0].id;
        project.content.automation={{pulse,AutomatedParameter::level,{{0,0},{400,100}}}};
        writeAuthoredProject(path,project);
        Session session;QVERIFY(session.loadProject(path));
        const auto id=session.trackIds()[0].toString();
        QVERIFY(session.renameTrack(id,"Swell"));QVERIFY(session.duplicateTrack(id));
        const auto copy=session.trackIds()[2].toString();
        QVERIFY(session.saveProject(path));
        auto saved=readAuthoredProject(path);
        QCOMPARE(saved.content.automation.size(),2u);
        QCOMPARE(saved.content.automation[0].track.value,pulse.value);
        QCOMPARE(QString::number(saved.content.automation[1].track.value),copy);
        QCOMPARE(saved.content.automation[1].points,saved.content.automation[0].points);
        QVERIFY(session.deleteTrack(id));QVERIFY(session.saveProject(path));
        saved=readAuthoredProject(path);
        QCOMPARE(saved.content.automation.size(),1u);QCOMPARE(QString::number(saved.content.automation[0].track.value),copy);
        session.undo();QCOMPARE(session.trackIds()[0].toString(),id);
        QVERIFY(session.saveProject(path));QCOMPARE(readAuthoredProject(path).content.automation.size(),2u);
        // The rendered export follows the lane: silence at the song start, full level after a bar.
        session.setTempo(120);QVERIFY(session.setMute(session.trackIds()[1].toString(),true));QVERIFY(session.setMute(copy,true));
        QVERIFY(session.exportAudio(directory.filePath("auto.wav"),1,2,0,2,"float32"));QTRY_VERIFY(!session.exporting());
        const auto w=readWav(directory.filePath("auto.wav"));const auto source=pulseClip(120);
        QCOMPARE(w.samples[0],0.0F);
        for(std::size_t f=1;f<96000;f+=4999)QCOMPARE(w.samples[2*f],static_cast<float>(static_cast<double>(f)/96000.0)*source.samples[f%source.samples.size()]);
        for(std::size_t f=96000;f<192000;f+=4999)QCOMPARE(w.samples[2*f],source.samples[f%source.samples.size()]);
    }
    void qmlAutomationEditing() {
        QTemporaryDir directory;
        Session session;session.setTempo(120);QVERIFY(session.setMute(session.trackIds()[1].toString(),true));
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* r,const QString& name)->QQuickItem* {
            if(r->objectName()==name && r->isVisible())return r;
            for(auto* c:r->childItems())if(auto* m=findItem(c,name))return m;
            return nullptr;
        };
        auto clickAt=[&](QQuickItem* item,QPointF local){QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(local).toPoint());};
        auto click=[&](const char* name){auto* item=findItem(window->contentItem(),name);QVERIFY2(item,name);clickAt(item,{item->width()/2,item->height()/2});};
        QVERIFY(!findItem(window->contentItem(),"automationLane0"));
        click("automationToggle0");
        auto* lane=findItem(window->contentItem(),"automationLane0");QVERIFY(lane);
        const auto level=[&]{return session.automationPoints(0,"level");};
        // Pointer: bottom-left adds (0, 0); one bar in at the top adds (400, 100).
        clickAt(lane,{1,lane->height()-1});
        QTRY_COMPARE(level(),(QVariantList{QVariant(QVariantList{0.0,0.0})}));QCOMPARE(session.undoText(),QString("Undo track 1 automation"));
        clickAt(lane,{lane->width()*400/1600,0});
        QTRY_COMPARE(level().size(),2);QCOMPARE(level()[1],QVariant(QVariantList{400.0,100.0}));
        // Clicking a point removes it; Undo brings it back.
        clickAt(lane,{lane->width()*400/1600,0});QTRY_COMPARE(level().size(),1);
        click("undoButton");QTRY_COMPARE(level().size(),2);
        // Keyboard: type a position in beats and a value, then Add point.
        auto* at=findItem(window->contentItem(),"automationAt0");at->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);for(const char c:QByteArray("2"))QTest::keyClick(window,c);QTest::keyClick(window,Qt::Key_Return);
        auto* value=findItem(window->contentItem(),"automationValue0");value->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);for(const char c:QByteArray("50"))QTest::keyClick(window,c);QTest::keyClick(window,Qt::Key_Return);
        click("automationAdd0");
        QTRY_COMPARE(level().size(),3);QCOMPARE(level()[1],QVariant(QVariantList{200.0,50.0}));
        click("undoButton");QTRY_COMPARE(level().size(),2);
        // Pan is a separate lane with its own range.
        findItem(window->contentItem(),"automationParameter0")->setProperty("currentIndex",1);
        QTRY_VERIFY(session.automationPoints(0,"pan").isEmpty());
        clickAt(lane,{lane->width()/2,0}); // top row: +100 (pan has no pixel row at exactly 0; use the value field)
        QTRY_COMPARE(session.automationPoints(0,"pan"),(QVariantList{QVariant(QVariantList{800.0,100.0})}));
        QVERIFY(window->grabWindow().save("desktop-automation.png"));
        click("clearAutomation0");QTRY_VERIFY(session.automationPoints(0,"pan").isEmpty());QCOMPARE(level().size(),2);
        // Refusals keep history.
        const auto undo=session.undoText();
        QVERIFY(!session.setAutomationPoint(session.trackIds()[0].toString(),"level",10,101));
        QVERIFY(!session.setAutomationPoint(session.trackIds()[0].toString(),"volume",10,50));
        QVERIFY(!session.setAutomationPoint(session.trackIds()[0].toString(),"level",10.5,50));
        QVERIFY(!session.removeAutomationPoint(session.trackIds()[0].toString(),"level",7));
        QCOMPARE(session.undoText(),undo);
        // Save/Open keeps the curve; the export follows it.
        QVERIFY(session.saveProject(directory.filePath("lane.json")));
        Session reopened;QVERIFY(reopened.loadProject(directory.filePath("lane.json")));
        QCOMPARE(reopened.automationPoints(0,"level"),level());
        QVERIFY(reopened.exportAudio(directory.filePath("lane.wav"),1,1,0,2,"float32"));QTRY_VERIFY(!reopened.exporting());
        const auto w=readWav(directory.filePath("lane.wav"));const auto source=pulseClip(120);
        for(std::size_t f=1;f<96000;f+=3001)QCOMPARE(w.samples[2*f],static_cast<float>(static_cast<double>(f)/96000.0)*source.samples[f%source.samples.size()]);
    }
    void qmlSynthEditing() {
        QTemporaryDir directory;
        Session session;session.setTempo(120);QVERIFY(session.setMute(session.trackIds()[0].toString(),true));
        QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("session",&session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* r,const QString& name)->QQuickItem* {
            if(r->objectName()==name && r->isVisible())return r;
            for(auto* c:r->childItems())if(auto* m=findItem(c,name))return m;
            return nullptr;
        };
        auto type=[&](const char* name,const char* text){auto* item=findItem(window->contentItem(),name);QVERIFY2(item,name);item->forceActiveFocus();
            QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);for(const char* c=text;*c;++c)QTest::keyClick(window,*c);QTest::keyClick(window,Qt::Key_Return);};
        auto* wave=findItem(window->contentItem(),"synthWaveform");QVERIFY(wave);wave->forceActiveFocus();QTest::keyClick(window,Qt::Key_Down);
        QTRY_COMPARE(session.synth()[0].toInt(),1);QCOMPARE(session.undoText(),QString("Undo synth waveform"));
        type("synthAttack","100");type("synthSustain","50");type("synthRelease","200");
        QCOMPARE(session.synth(),(QVariantList{1,100,0,50,200}));QCOMPARE(session.undoText(),QString("Undo synth release"));
        QVERIFY(!session.setSynth("sustain",101));QVERIFY(!session.setSynth("volume",1));QCOMPARE(session.undoText(),QString("Undo synth release"));
        session.undo();QCOMPARE(session.synth()[4].toInt(),0);session.redo();
        QVERIFY(window->grabWindow().save("desktop-synth.png"));
        // Export: the first step (C4, gate 75%) through triangle and the envelope.
        QVERIFY(session.exportAudio(directory.filePath("synth.wav"),1,1,0,1,"float32"));QTRY_VERIFY(!session.exporting());
        const auto w=readWav(directory.filePath("synth.wav"));
        const double step=440.0*std::exp2((60.0-69.0)/12.0)/48000.0;double phase=0,worst=0;
        for(std::size_t t=0;t<24000;++t) {
            const double env=t<4800 ? static_cast<double>(static_cast<float>(t)/4800.0F) : t<18000 ? 0.5 : 0.5*(1-static_cast<double>(t-18000)/9600.0);
            // Velocity 70% (x0.1 instrument scale), level 25%; centered, so mono equals each channel.
            const double expected=(4*std::abs(phase-0.5)-1)*0.07*env*0.25;
            worst=std::max(worst,std::abs(w.samples[t]-expected));
            phase+=step;phase-=std::floor(phase);
        }
        QVERIFY2(worst<1e-5,qPrintable(QString("synth export differs by %1").arg(worst)));
        // Save/Open keeps the synth; another MIDI track has its own default synth.
        QVERIFY(session.saveProject(directory.filePath("synth.json")));
        {QFile f(directory.filePath("synth.json"));QVERIFY(f.open(QIODevice::ReadOnly));QVERIFY(f.readAll().contains("synth-v1"));}
        Session reopened;QVERIFY(reopened.loadProject(directory.filePath("synth.json")));QCOMPARE(reopened.synth(),(QVariantList{1,100,0,50,200}));
        QVERIFY(reopened.addTrack("midi"));QVERIFY(reopened.editPattern(reopened.trackIds()[2].toString()));
        QCOMPARE(reopened.synth(),(QVariantList{0,0,0,100,0}));
        QVERIFY(reopened.duplicateTrack(reopened.trackIds()[1].toString()));QVERIFY(reopened.editPattern(reopened.trackIds()[3].toString()));
        QCOMPARE(reopened.synth(),(QVariantList{1,100,0,50,200})); // duplicates copy the synth
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
