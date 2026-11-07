#include <QElapsedTimer>
#include <QSGRendererInterface>
#include <iostream>
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

// Drives the production QML with synthetic pointer and keyboard input.
struct QmlUi {
    QQmlApplicationEngine engine;QQuickWindow* window=nullptr;
    explicit QmlUi(Session& session){engine.rootContext()->setContextProperty("session",&session);engine.load(QUrl("qrc:/ui/Main.qml"));
        window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());}
    QQuickItem* find(QQuickItem* r,const QString& name){if(r->objectName()==name && r->isVisible())return r;
        for(auto* c:r->childItems())if(auto* m=find(c,name))return m;return nullptr;}
    QQuickItem* item(const QString& name){auto* i=find(window->contentItem(),name);if(!i)throw std::runtime_error(name.toStdString());return i;}
    void click(const QString& name){auto* i=item(name);QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,i->mapToScene(QPointF(i->width()/2,i->height()/2)).toPoint());}
    void type(const QString& name,const QString& text,bool enter=false){item(name)->forceActiveFocus();QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        for(const QChar c:text)QTest::keyClick(window,c.toLatin1());if(enter)QTest::keyClick(window,Qt::Key_Return);}
    void keys(const QString& name,Qt::Key key,int times){item(name)->forceActiveFocus();for(int i=0;i<times;++i)QTest::keyClick(window,key);}
    QString status(){return item("statusMessage")->property("text").toString();}
    void exportTo(Session& session,const QString& path,const QString& bars){type("exportFile",path);type("exportBars",bars,true);click("exportWavButton");}
};
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
        // Scene edits apply while playing and leave playback running.
        QVERIFY(session.copyScene(first));QCOMPARE(session.sceneIds().size(),qsizetype(3));QVERIFY(session.message().contains("while playing"));
        session.undo();QCOMPARE(session.sceneIds().size(),qsizetype(2));session.tick();session.tick();QVERIFY(session.running());
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
        const auto scenes=session.sceneIds().size();QVERIFY(session.createScene());QCOMPARE(session.sceneIds().size(),scenes+1);
        session.undo();QCOMPARE(session.sceneIds().size(),scenes);QCOMPARE(session.sceneId(),empty);session.tick();session.tick();
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
        QVERIFY(session.setSceneIncluded(tracks[0].toString(),false));QCOMPARE(session.sceneIncluded(),(QVariantList{false,false}));
        session.undo();QCOMPARE(session.sceneIncluded(),(QVariantList{true,false}));session.tick();session.tick();
        QCOMPARE(session.activeClipIds()[1],clips[1]);
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
    // Supported edits and Undo apply while playing without resetting the
    // playhead; tempo is refused; deleted tracks stop and stay stopped.
    void playbackEditsAndUndo() {
        Session session;const auto tracks=session.trackIds();const auto clips=session.clipIds();
        QVERIFY(session.launchClip(clips[0].toString()));QVERIFY(session.launchClip(clips[1].toString()));session.play();
        for(int i=0;i<60;++i)session.tick(); // 28800 frames at 120 BPM
        QVERIFY(session.running());QCOMPARE(session.activeClipIds(),clips);QCOMPARE(session.position(),QString("1.2"));
        const auto applied=session.playbackEdits();
        session.setLevel(0,40);QCOMPARE(session.levels()[0].toInt(),40);QVERIFY(session.message().contains("while playing"));
        session.tick();QCOMPARE(session.playbackEdits(),applied+1);QVERIFY(session.running());
        QCOMPARE(session.activeClipIds(),clips);QCOMPARE(session.position(),QString("1.2"));
        // A burst supersedes edits that were not yet offered; the last one wins.
        for(int level=41;level<=50;++level)session.setLevel(0,level);
        for(int i=0;i<4;++i)session.tick();
        QCOMPARE(session.levels()[0].toInt(),50);QVERIFY(session.playbackEdits()<=applied+3);QCOMPARE(session.undoText(),QString("Undo track 1 level"));
        // Controls wait until a pending edit has been offered.
        session.setLevel(0,61);session.setLevel(0,62);
        QVERIFY(!session.launchClip(clips[0].toString()));QVERIFY(session.message().contains("Applying the last edit"));
        session.tick();session.tick();QVERIFY(session.launchClip(clips[0].toString()));
        // Tempo is not a playback edit.
        const auto undo=session.undoText();session.setTempo(90);
        QCOMPARE(session.tempo(),120);QCOMPARE(session.undoText(),undo);QVERIFY(session.message().contains("Stop transport to change the tempo"));
        // Deleting a playing track stops its notes; Undo restores the track, not its playback.
        QVERIFY(session.deleteTrack(tracks[1].toString()));session.tick();session.tick();
        QCOMPARE(session.trackCount(),1);QCOMPARE(session.activeClipIds(),(QVariantList{clips[0]}));QVERIFY(session.running());
        session.undo();QVERIFY(session.message().contains("while playing"));session.tick();session.tick();
        QCOMPARE(session.trackCount(),2);QCOMPARE(session.activeClipIds(),(QVariantList{clips[0],QString{}}));
        session.redo();session.tick();session.tick();QCOMPARE(session.trackCount(),1);session.undo();session.tick();session.tick();
        // Stopped edits keep rebuilding at the start.
        session.stop();session.tick();QVERIFY(!session.running());
        session.setLevel(0,70);QVERIFY(session.message().contains("prepared at the start"));QCOMPARE(session.position(),QString("1.1"));
        QCOMPARE(session.playbackEdits(),std::uint64_t(0));
    }
    // A flooded control queue refuses controls visibly, keeps a live edit for
    // later, and never blocks Silence now; everything recovers afterward.
    void editBurstBackpressureAndPanic() {
        Session session;const auto clips=session.clipIds();
        QVERIFY(session.launchClip(clips[0].toString()));session.play();session.tick();QVERIFY(session.running());
        int queued=0;while(session.launchClip(clips[0].toString()))++queued; // preview renders only on tick
        QCOMPARE(queued,int(fl_live::Engine::command_capacity));QVERIFY(session.message().contains("Control queue full"));
        session.stop();QVERIFY(session.message().contains("Control queue full"));
        session.setLevel(0,30);QCOMPARE(session.levels()[0].toInt(),30);QVERIFY(session.message().contains("while playing"));
        QVERIFY(!session.launchClip(clips[0].toString()));QVERIFY(session.message().contains("Applying the last edit"));
        const auto applied=session.playbackEdits();
        session.panic();session.tick();QVERIFY(!session.running());QVERIFY(!session.pending());
        session.tick();QCOMPARE(session.playbackEdits(),applied+1); // handed over once the queue had room
        QCOMPARE(session.undoText(),QString("Undo track 1 level"));
        QVERIFY(session.launchScene(session.sceneId(),"immediate"));session.play();session.tick();QVERIFY(session.running());
        QCOMPARE(session.activeClipIds()[0],clips[0]);session.stop();session.tick();
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
        // An edit while the launch is queued keeps the queued launch.
        QVERIFY(session.renameScene(id,"Pending"));session.undo();QCOMPARE(session.sceneName(),QString("Verse"));session.tick();session.tick();
        QCOMPARE(session.sceneState(),QString("Stopped · actions queued"));QVERIFY(!session.dirty());
        session.play();session.tick();QCOMPARE(session.sceneState(),QString("Active"));QCOMPARE(session.activeClipIds(),clips);
        QVERIFY(session.renameScene(id,"Playing"));session.undo();session.tick();session.tick();QCOMPARE(session.activeClipIds(),clips);
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
        QVERIFY(session.renameClip(clips[1].toString(),"Pending"));QCOMPARE(session.clipNames()[1].toString(),QString("Pending"));
        session.tick();QVERIFY(session.running());QCOMPARE(session.activeClipIds()[1],clips[1]);
        QVERIFY(session.renameTrack(tracks[0].toString(),"Running"));session.tick();QCOMPARE(session.activeClipIds()[1],clips[1]);
        session.panic();session.tick();QCOMPARE(session.activeClipIds()[1].toString(),QString{});
        session.undo();session.undo();QCOMPARE(session.trackNames()[0].toString(),QString("Drums"));QVERIFY(!session.dirty());
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
        session.play();QVERIFY(session.setSolo(audio,false));QCOMPARE(session.solos(),(QVariantList{false,true}));session.tick();
        QVERIFY(session.running());session.undo();QCOMPARE(session.solos(),(QVariantList{true,true}));
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
        // Track edits apply while playing; the playing clip follows its track.
        QVERIFY(!findItem(window->contentItem(),"tempoSpin")->property("enabled").toBool());
        click("deleteTrack0");QTRY_COMPARE(session.trackCount(),4);session.tick();session.tick();
        QVERIFY(session.running());QCOMPARE(session.activeClips()[1].toInt(),0);
        click("undoButton");QTRY_COMPARE(session.trackCount(),5);session.tick();session.tick();QCOMPARE(session.activeClips()[2].toInt(),0);
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
    // #65: create, save, move, reopen, repair, export and cancel, all through
    // the QML. Every export is decoded independently and compared with the
    // first one; refused exports leave existing files and the document alone.
    void qmlRecoveryExportJourney() {
        QTemporaryDir root;const auto bytesOf=[](const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();};
        const auto folder=root.filePath("song"),moved=root.filePath("moved");QVERIFY(QDir().mkpath(folder));
        std::vector<qint16> loop(4800*2);
        for(std::size_t i=0;i<4800;++i){loop[2*i]=static_cast<qint16>(9000*std::sin(static_cast<double>(i)/23));loop[2*i+1]=static_cast<qint16>(-7000*std::sin(static_cast<double>(i)/31));}
        writeWav(folder+"/loop.wav",2,48000,loop);
        const auto first=root.filePath("first.wav");QByteArray firstBytes;
        {
            Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
            ui.type("audioPath0",folder+"/loop.wav");ui.click("importAudio0");QTRY_VERIFY(!session.importing());
            QCOMPARE(session.trackSources()[0].toString(),QString("loop.wav · 0.10 s · stereo"));
            ui.keys("level0",Qt::Key_Down,20);ui.keys("level1",Qt::Key_Up,5);
            ui.type("projectFile",folder+"/song.json");ui.click("saveProjectButton");QVERIFY2(!session.dirty(),qPrintable(session.message()));
            ui.keys("exportFormat",Qt::Key_Down,2);QCOMPARE(ui.item("exportFormat")->property("currentIndex").toInt(),2);
            ui.exportTo(session,first,"2");QTRY_VERIFY(!session.exporting());QVERIFY2(QFile::exists(first),qPrintable(ui.status()));
            firstBytes=bytesOf(first);QVERIFY(ui.window->close());
        }
        const auto reference=readWav(first);QCOMPARE(reference.format,3);QCOMPARE(reference.channels,2);
        QVERIFY(std::any_of(reference.samples.begin(),reference.samples.end(),[](float v){return v!=0;}));
        // Move the whole project folder: the relative reference finds the audio.
        QVERIFY(QDir().rename(folder,moved));
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        ui.type("projectFile",moved+"/song.json");ui.click("openProjectButton");
        QTRY_VERIFY(ui.status().contains("Found moved audio"));QCOMPARE(session.trackSources()[0].toString(),QString("loop.wav · 0.10 s · stereo"));
        ui.keys("exportFormat",Qt::Key_Down,2);
        ui.exportTo(session,root.filePath("relocated.wav"),"2");QTRY_VERIFY(!session.exporting());
        QVERIFY(readWav(root.filePath("relocated.wav")).samples==reference.samples);
        // The new location is an unsaved change, so Open refuses until it is saved.
        QVERIFY(session.dirty());ui.click("openProjectButton");QTRY_VERIFY(ui.status().contains("Save your changes"));
        ui.click("saveProjectButton");QVERIFY(!session.dirty());
        // Remove the audio: the reopened clip is missing until explicitly relinked.
        const auto copy=root.filePath("elsewhere/loop-copy.wav");QVERIFY(QDir().mkpath(root.filePath("elsewhere")));
        QVERIFY(QFile::copy(moved+"/loop.wav",copy));QVERIFY(QFile::remove(moved+"/loop.wav"));
        ui.click("openProjectButton");QTRY_VERIFY2(ui.status().contains("Missing audio plays as silence"),qPrintable(ui.status()));
        QCOMPARE(session.trackSources()[0].toString(),QString("Missing: loop.wav"));QVERIFY(!session.dirty());
        ui.type("audioPath0",copy);ui.click("relinkAudio0");
        QTRY_COMPARE(session.trackSources()[0].toString(),QString("loop-copy.wav · 0.10 s · stereo"));QVERIFY(session.dirty());
        ui.exportTo(session,root.filePath("relinked.wav"),"2");QTRY_VERIFY(!session.exporting());
        QVERIFY(readWav(root.filePath("relinked.wav")).samples==reference.samples);
        // Refused exports: an existing file is never replaced; a missing folder fails. The document stays as it was.
        const auto undo=session.undoText();
        ui.exportTo(session,first,"2");QTRY_VERIFY(!session.exporting());
        QCOMPARE(bytesOf(first),firstBytes);QVERIFY(session.dirty());QCOMPARE(session.undoText(),undo);
        const auto refusedExisting=ui.status();QVERIFY(!refusedExisting.isEmpty());
        ui.exportTo(session,root.filePath("no-such-folder/x.wav"),"2");QTRY_VERIFY(!session.exporting());
        QVERIFY(!QFile::exists(root.filePath("no-such-folder/x.wav")));QVERIFY(session.dirty());QCOMPARE(session.undoText(),undo);
        // Cancel a long export through its button: no file or partial file remains.
        const auto longPath=root.filePath("long.wav");
        ui.exportTo(session,longPath,"64");QVERIFY(session.exporting());ui.click("cancelExportButton");QTRY_VERIFY(!session.exporting());
        QVERIFY(!QFile::exists(longPath));QVERIFY(!QFile::exists(longPath+".part"));QVERIFY(ui.status().contains("cancel",Qt::CaseInsensitive));
        // Save the repair and reopen in a fresh session: the same render.
        ui.click("saveProjectButton");QVERIFY(!session.dirty());QVERIFY(ui.window->close());
        Session reopened;QmlUi again(reopened);QVERIFY(again.window && QTest::qWaitForWindowExposed(again.window));
        again.type("projectFile",moved+"/song.json");again.click("openProjectButton");
        QTRY_COMPARE(reopened.trackSources()[0].toString(),QString("loop-copy.wav · 0.10 s · stereo"));QCOMPARE(reopened.levels(),session.levels());
        again.keys("exportFormat",Qt::Key_Down,2);
        again.exportTo(reopened,root.filePath("reopened.wav"),"2");QTRY_VERIFY(!reopened.exporting());
        QVERIFY(readWav(root.filePath("reopened.wav")).samples==reference.samples);
        QVERIFY(again.window->grabWindow().save("desktop-recovery-journey.png"));
    }
    // Epic #12 journey through the QML: automate and edit while playing,
    // refuse tempo, undo, panic, then Save, reopen and export the lane.
    void qmlPlaybackEditJourney() {
        QTemporaryDir directory;const auto project=directory.filePath("live.json");
        {
            Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
            ui.click("clip0");ui.click("clip1");ui.click("playButton");QTRY_VERIFY(session.running());
            QTRY_COMPARE(session.activeClipIds(),session.clipIds());
            QVERIFY(!ui.item("tempoSpin")->property("enabled").toBool());
            // Draw a one-bar level ramp on the pulse track while it plays.
            ui.click("automationToggle0");auto* lane=ui.item("automationLane0");
            QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,lane->mapToScene(QPointF(1,lane->height()-1)).toPoint());
            QTRY_COMPARE(session.automationPoints(0,"level").size(),1);QVERIFY(ui.status().contains("while playing"));
            QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,lane->mapToScene(QPointF(lane->width()*400/1600,0)).toPoint());
            QTRY_COMPARE(session.automationPoints(0,"level").size(),2);
            QTRY_VERIFY(session.playbackEdits()>=2);QVERIFY(session.running());QCOMPARE(session.activeClipIds(),session.clipIds());
            // Mixer and pattern edits while playing; Undo through the button.
            ui.click("mute1");QTRY_COMPARE(session.mutes()[1].toBool(),true);
            ui.click("editPattern1");ui.click("step0");const bool before=session.stepEnabled()[0].toBool();
            ui.click("stepEnabled");QTRY_COMPARE(session.stepEnabled()[0].toBool(),!before);
            ui.click("undoButton");QTRY_COMPARE(session.stepEnabled()[0].toBool(),before);QVERIFY(ui.status().contains("while playing"));
            QVERIFY(session.running());QCOMPARE(session.activeClipIds(),session.clipIds());
            // Silence now through its shortcut, then Save the automated, muted project.
            ui.window->requestActivate();QTest::keyClick(ui.window,Qt::Key_Period,Qt::ControlModifier);QTRY_VERIFY(!session.running());
            QTRY_VERIFY(session.canEdit());
            ui.type("projectFile",project);ui.click("saveProjectButton");QVERIFY2(!session.dirty(),qPrintable(ui.status()));
            QVERIFY(ui.window->close());
        }
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        ui.type("projectFile",project);ui.click("openProjectButton");QTRY_COMPARE(session.automationPoints(0,"level").size(),2);
        QCOMPARE(session.automationPoints(0,"level")[1],QVariant(QVariantList{400.0,100.0}));QCOMPARE(session.mutes()[1].toBool(),true);
        ui.keys("exportFormat",Qt::Key_Down,2);ui.exportTo(session,directory.filePath("live.wav"),"2");QTRY_VERIFY(!session.exporting());
        const auto w=readWav(directory.filePath("live.wav"));const auto source=pulseClip(120);
        QCOMPARE(w.samples[0],0.0F);
        for(std::size_t f=1;f<96000;f+=4999)QCOMPARE(w.samples[2*f],static_cast<float>(static_cast<double>(f)/96000.0)*source.samples[f%source.samples.size()]);
        for(std::size_t f=96000;f<192000;f+=4999)QCOMPARE(w.samples[2*f],source.samples[f%source.samples.size()]);
    }
    // #35: converting the step pattern, adding, selecting and deleting notes,
    // refusals, stable IDs, Undo/Redo and Save/Open.
    void pianoRollEditing() {
        QTemporaryDir directory;Session session;QCOMPARE(session.patternTrackIndex(),1);QVERIFY(!session.pianoMode());
        QVERIFY(session.exportAudio(directory.filePath("steps.wav"),1,1,0,2,"float32"));QTRY_VERIFY(!session.exporting());
        QVERIFY(session.usePianoRoll(true));QVERIFY(session.pianoMode());QCOMPARE(session.undoText(),QString("Undo piano roll"));
        QCOMPARE(session.pianoLength(),384);QCOMPARE(session.trackSources()[1].toString(),QString("Piano roll · 4 notes"));
        const auto notes=session.pianoNotes();QCOMPARE(notes.size(),4);
        const auto note=[&](int i){return notes[i].toMap();};
        QCOMPARE(note(0)["id"].toString(),QString("1"));QCOMPARE(note(0)["start"].toInt(),0);QCOMPARE(note(0)["length"].toInt(),72);
        QCOMPARE(note(3)["start"].toInt(),288);QCOMPARE(note(3)["pitch"].toInt(),72);QCOMPARE(note(3)["velocity"].toInt(),70);
        // The converted notes sound exactly like the pattern they came from.
        QVERIFY(session.exportAudio(directory.filePath("notes.wav"),1,1,0,2,"float32"));QTRY_VERIFY(!session.exporting());
        QVERIFY(readWav(directory.filePath("notes.wav")).samples==readWav(directory.filePath("steps.wav")).samples);
        // Add selects the new note; its ID comes from the clip counter.
        QVERIFY(session.addNote(24,24,62,90));QCOMPARE(session.selectedNotes(),(QVariantList{"5"}));QCOMPARE(session.undoText(),QString("Undo add note"));
        QCOMPARE(session.pianoNotes()[1].toMap()["id"].toString(),QString("5")); // ordered by start
        // Refusals leave notes and history unchanged.
        const auto before=session.pianoNotes();const auto undo=session.undoText();
        QVERIFY(!session.addNote(384,24,60,80));QVERIFY(!session.addNote(360,48,60,80));QVERIFY(!session.addNote(0,24,128,80));
        QVERIFY(!session.addNote(0,24,60,0));QVERIFY(!session.addNote(0,0,60,80));QVERIFY(!session.addNote(-24,24,60,80));
        for(int n=0;n<15;++n)QVERIFY(session.addNote(0,24,30+n,80)); // with note 1, 16 sound at tick 0
        QVERIFY(!session.addNote(0,24,90,80));QVERIFY(session.message().contains("16 notes"));
        for(int n=0;n<15;++n)session.undo();
        QCOMPARE(session.pianoNotes(),before);QCOMPARE(session.undoText(),undo);
        QVERIFY(!session.deleteNotes({}));QVERIFY(!session.deleteNotes({"99"}));QCOMPARE(session.pianoNotes(),before);
        // Selection is view state keyed by ID; unrelated edits keep IDs and selection.
        session.selectNote("1",false);session.selectNote("3",true);QCOMPARE(session.selectedNotes(),(QVariantList{"1","3"}));
        session.selectNote("1",true);QCOMPARE(session.selectedNotes(),(QVariantList{"3"}));session.selectNote("1",true);
        session.setLevel(0,40);QCOMPARE(session.selectedNotes(),(QVariantList{"1","3"}));QCOMPARE(session.pianoNotes(),before);
        session.selectNote("99",false);QVERIFY(session.message().contains("Unknown note"));QCOMPARE(session.selectedNotes(),(QVariantList{"1","3"}));
        QVERIFY(session.deleteNotes(session.selectedNotes()));QCOMPARE(session.pianoNotes().size(),3);QCOMPARE(session.undoText(),QString("Undo delete notes"));
        QVERIFY(session.selectedNotes().isEmpty());
        session.undo();QCOMPARE(session.pianoNotes(),before);session.redo();QCOMPARE(session.pianoNotes().size(),3);
        // Deleted IDs 1 and 3 are not reissued; Undo restored the counter with the notes it removed.
        QVERIFY(session.addNote(96,24,61,80));QCOMPARE(session.selectedNotes(),(QVariantList{"6"}));
        // Turning the piano roll off plays the pattern again; Undo brings the same notes back.
        const auto withNotes=session.pianoNotes();
        QVERIFY(session.usePianoRoll(false));QVERIFY(!session.pianoMode());QVERIFY(session.pianoNotes().isEmpty());QCOMPARE(session.stepCount(),4);
        session.undo();QCOMPARE(session.pianoNotes(),withNotes);
        // Save/Open keeps positions, lengths, pitches, velocities and IDs.
        const auto path=directory.filePath("piano.json");QVERIFY(session.saveProject(path));
        Session reopened;QVERIFY(reopened.loadProject(path));QVERIFY(reopened.pianoMode());QCOMPARE(reopened.pianoNotes(),withNotes);
        QVERIFY(reopened.addNote(144,24,59,80));QCOMPARE(reopened.selectedNotes(),(QVariantList{"7"}));
        // Choosing another MIDI clip clears the selection.
        QVERIFY(reopened.addTrack("midi"));QVERIFY(reopened.editPattern(reopened.trackIds()[2].toString()));QVERIFY(reopened.selectedNotes().isEmpty());
        QVERIFY(!reopened.pianoMode());QVERIFY(!reopened.addNote(0,24,60,80));QVERIFY(reopened.message().contains("piano roll"));
    }
    // #35 through the QML: pointer add/select/Shift-select, Delete key, Undo
    // button and keyboard entry.
    void qmlPianoRollInput() {
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        ui.click("pianoMode");QTRY_VERIFY(session.pianoMode());
        auto* roll=ui.item("pianoRoll");QVERIFY(roll->height()>=120);
        QVERIFY(roll->mapToScene(QPointF(0,roll->height())).y()<=ui.window->height());
        QVERIFY(ui.item("addNoteButton")->mapToScene(QPointF(0,ui.item("addNoteButton")->height())).y()<=ui.window->height());
        const auto at=[&](int tick,int pitch){
            const int base=roll->property("base").toInt(),rows=roll->property("rows").toInt();const double row=roll->property("rowHeight").toDouble();
            return roll->mapToScene(QPointF((tick+0.5)*roll->width()/session.pianoLength(),(base+rows-1-pitch+0.5)*row)).toPoint();};
        QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,at(200,62)); // snaps to tick 192
        QTRY_COMPARE(session.pianoNotes().size(),5);
        QVariantMap added;for(const auto& n:session.pianoNotes())if(n.toMap()["pitch"].toInt()==62)added=n.toMap();
        QCOMPARE(added["start"].toInt(),192);QCOMPARE(added["length"].toInt(),24);QCOMPARE(session.selectedNotes(),(QVariantList{added["id"]}));
        QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,at(10,60));QTRY_COMPARE(session.selectedNotes(),(QVariantList{"1"}));
        QTest::mouseClick(ui.window,Qt::LeftButton,Qt::ShiftModifier,at(195,62));
        QTRY_COMPARE(session.selectedNotes().size(),2);
        QTRY_COMPARE(ui.window->grabWindow().pixelColor(at(10,60)),QColor("#e4bc75")); // drawn in the selected colour
        QTest::keyClick(ui.window,Qt::Key_Delete);QTRY_COMPARE(session.pianoNotes().size(),3);
        ui.click("undoButton");QTRY_COMPARE(session.pianoNotes().size(),5);
        ui.window->requestActivate();ui.item("pianoRoll")->forceActiveFocus();QTest::keyClick(ui.window,Qt::Key_Escape);QTRY_VERIFY(session.selectedNotes().isEmpty());
        // Keyboard entry: start in sixteenths, length, pitch, velocity.
        ui.type("noteStart","4",true);ui.type("noteLength","2",true);ui.type("notePitch","65",true);ui.type("noteVelocity","90",true);
        ui.click("addNoteButton");QTRY_COMPARE(session.pianoNotes().size(),6);
        QVariantMap typed;for(const auto& n:session.pianoNotes())if(n.toMap()["pitch"].toInt()==65)typed=n.toMap();
        QCOMPARE(typed["start"].toInt(),96);QCOMPARE(typed["length"].toInt(),48);QCOMPARE(typed["velocity"].toInt(),90);
        ui.click("pianoOctaveUp");QTRY_VERIFY(ui.item("pianoRange")->property("text").toString().startsWith("Pitches 60–83"));
        QVERIFY(ui.window->grabWindow().save("desktop-piano-roll.png"));
    }
    // #36: move, resize and velocity with an all-or-nothing policy, and the
    // rendered timing of a moved note at a non-dividing tempo.
    void pianoRollMoveResize() {
        QTemporaryDir directory;Session session;session.setTempo(137);QVERIFY(session.setMute(session.trackIds()[0].toString(),true));
        QVERIFY(session.usePianoRoll(true));
        const auto note=[&](const QString& id){for(const auto& n:session.pianoNotes())if(n.toMap()["id"].toString()==id)return n.toMap();return QVariantMap{};};
        QVERIFY(session.moveNotes({"2","3"},24,2));QCOMPARE(session.undoText(),QString("Undo move notes"));
        QCOMPARE(note("2")["start"].toInt(),120);QCOMPARE(note("2")["pitch"].toInt(),66);QCOMPARE(note("3")["start"].toInt(),216);QCOMPARE(note("3")["pitch"].toInt(),69);
        // All or nothing: one note leaving the clip or the pitch range refuses the whole edit.
        const auto before=session.pianoNotes();const auto undo=session.undoText();
        QVERIFY(!session.moveNotes({"4","1"},-24,0));QVERIFY(session.message().contains("nothing moved"));
        QVERIFY(!session.moveNotes({"4"},48,0));QVERIFY(!session.moveNotes({"4"},0,56));QVERIFY(!session.moveNotes({"1"},0,-61));
        QVERIFY(!session.resizeNotes({"1"},-72));QVERIFY(!session.resizeNotes({"2","4"},48));QVERIFY(session.message().contains("nothing changed"));
        QVERIFY(!session.setNotesVelocity({"1"},0));QVERIFY(!session.setNotesVelocity({"1"},101));QVERIFY(!session.moveNotes({"9"},24,0));QVERIFY(!session.moveNotes({},24,0));
        QCOMPARE(session.pianoNotes(),before);QCOMPARE(session.undoText(),undo);
        QVERIFY(session.resizeNotes({"1"},24));QCOMPARE(note("1")["length"].toInt(),96);QCOMPARE(session.undoText(),QString("Undo resize note"));
        QVERIFY(session.setNotesVelocity({"1","2"},100));QCOMPARE(note("2")["velocity"].toInt(),100);QCOMPARE(session.undoText(),QString("Undo note velocity"));
        // Overlaps are allowed, even on one pitch: both notes sound. The 16-voice limit still applies.
        QVERIFY(session.moveNotes({"2"},-120,-6));QCOMPARE(note("2")["start"].toInt(),0);QCOMPARE(note("2")["pitch"].toInt(),60);
        session.undo();
        for(int n=0;n<15;++n)QVERIFY(session.addNote(288,24,30+n,80)); // with note 4, 16 sound at tick 288
        QVERIFY(!session.moveNotes({"3"},72,0));QVERIFY(session.message().contains("16 notes"));
        for(int n=0;n<15;++n)session.undo();
        // Undo walks back each edit; IDs never change.
        session.undo();session.undo();QCOMPARE(note("1")["length"].toInt(),72);session.undo();QCOMPARE(note("2")["start"].toInt(),96);
        // Timing: one square note moved off the grid by 37 ticks at 137 BPM.
        QVERIFY(session.deleteNotes({"2","3","4"}));QVERIFY(session.setSynth("waveform",2));QVERIFY(session.moveNotes({"1"},37,0));
        QVERIFY(session.exportAudio(directory.filePath("moved.wav"),1,1,0,2,"float32"));QTRY_VERIFY(!session.exporting());
        const auto w=readWav(directory.filePath("moved.wav"));
        const auto frame=[](std::uint64_t ticks){return (ticks*30000+136)/137;}; // ceil(ticks * 48000 * 60 / (137 * 96))
        const auto start=frame(37),end=frame(37+72);QCOMPARE(start,std::uint64_t(8103));QCOMPARE(end,std::uint64_t(23869));
        QCOMPARE(w.samples[2*(start-1)],0.0F);QVERIFY(w.samples[2*start]!=0.0F);QVERIFY(w.samples[2*(end-1)]!=0.0F);QCOMPARE(w.samples[2*end],0.0F);
    }
    // #36 through the QML: drag to move, drag the edge to resize, arrow keys,
    // snap and the selection's velocity.
    void qmlPianoRollDragAndKeys() {
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        ui.click("pianoMode");QTRY_VERIFY(session.pianoMode());
        auto* roll=ui.item("pianoRoll");
        const auto at=[&](double tick,int pitch){
            const int base=roll->property("base").toInt(),rows=roll->property("rows").toInt();const double row=roll->property("rowHeight").toDouble();
            return roll->mapToScene(QPointF(tick*roll->width()/session.pianoLength(),(base+rows-1-pitch+0.5)*row)).toPoint();};
        const auto rowPixels=roll->property("rowHeight").toDouble();
        const auto drag=[&](QPoint from,QPoint to){
            QTest::mousePress(ui.window,Qt::LeftButton,Qt::NoModifier,from);
            for(int step=1;step<=6;++step)QTest::mouseMove(ui.window,from+(to-from)*step/6);
            QTest::mouseRelease(ui.window,Qt::LeftButton,Qt::NoModifier,to);
        };
        const auto note=[&](const QString& id){for(const auto& n:session.pianoNotes())if(n.toMap()["id"].toString()==id)return n.toMap();return QVariantMap{};};
        // Drag note 2 (beat 2, pitch 64) right by two sixteenths and up two semitones.
        const auto from=at(130,64);drag(from,from+QPoint(qRound(48*roll->width()/session.pianoLength()),-qRound(2*rowPixels)));
        QTRY_COMPARE(note("2")["start"].toInt(),144);QCOMPARE(note("2")["pitch"].toInt(),66);QCOMPARE(session.selectedNotes(),(QVariantList{"2"}));
        // Drag the right edge of note 3 (192..264) one sixteenth longer.
        const auto edge=at(263,67);drag(edge,edge+QPoint(qRound(24*roll->width()/session.pianoLength()),0));
        QTRY_COMPARE(note("3")["length"].toInt(),96);QCOMPARE(note("3")["start"].toInt(),192);
        // A drag that would leave the clip is refused and leaves the note alone.
        const auto third=at(200,67);drag(third,third+QPoint(qRound(120*roll->width()/session.pianoLength()),0)); // 192..288 would end at 408 of 384
        QTRY_VERIFY2(ui.status().contains("nothing moved"),qPrintable(ui.status()));QCOMPARE(note("3")["start"].toInt(),192);
        // Keyboard: Right moves by the snap, Shift+Right resizes, Up/Shift+Up change pitch.
        QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,at(10,60));QTRY_COMPARE(session.selectedNotes(),(QVariantList{"1"}));
        QTest::keyClick(ui.window,Qt::Key_Right);QTRY_COMPARE(note("1")["start"].toInt(),24);
        QTest::keyClick(ui.window,Qt::Key_Right,Qt::ShiftModifier);QTRY_COMPARE(note("1")["length"].toInt(),96);
        QTest::keyClick(ui.window,Qt::Key_Up);QTRY_COMPARE(note("1")["pitch"].toInt(),61);
        QTest::keyClick(ui.window,Qt::Key_Up,Qt::ShiftModifier);QTRY_COMPARE(note("1")["pitch"].toInt(),73);
        // Snap Off moves by one tick; the velocity box edits the selection.
        ui.keys("pianoSnap",Qt::Key_Down,3);QCOMPARE(ui.item("pianoSnap")->property("currentIndex").toInt(),3);
        ui.item("pianoRoll")->forceActiveFocus();QTest::keyClick(ui.window,Qt::Key_Left);QTRY_COMPARE(note("1")["start"].toInt(),23);
        ui.type("selectedNotesVelocity","40",true);QTRY_COMPARE(note("1")["velocity"].toInt(),40);
        ui.click("undoButton");QTRY_COMPARE(note("1")["velocity"].toInt(),70);
    }
    // #38: shortening keeps notes but silences them, lengthening restores
    // them, Loop off plays once; refusals and persistence.
    void pianoRollLengthLoop() {
        QTemporaryDir directory;
        const auto exported=[&](Session& s,const QString& name,int bars){
            const auto path=directory.filePath(name);if(!s.exportAudio(path,1,bars,0,2,"float32"))throw std::runtime_error("export refused");
            for(int i=0;i<1000 && s.exporting();++i)QTest::qWait(10);
            if(s.exporting())throw std::runtime_error("export did not finish");
            return readWav(path).samples;};
        const auto prepare=[](Session& s){s.setMute(s.trackIds()[0].toString(),true);s.setSynth("waveform",2);s.usePianoRoll(true);};
        Session session;prepare(session);const auto full=exported(session,"full.wav",2);
        QVERIFY(session.setPianoLength(192));QCOMPARE(session.pianoLength(),192);QCOMPARE(session.undoText(),QString("Undo clip length"));
        QCOMPARE(session.pianoNotes().size(),4);QCOMPARE(session.trackSources()[1].toString(),QString("Piano roll · 4 notes"));
        QCOMPARE(session.pianoNotes()[1].toMap()["sounding"].toBool(),true);QCOMPARE(session.pianoNotes()[2].toMap()["sounding"].toBool(),false);
        // Same sound as a two-beat clip whose later notes were deleted.
        Session reference;prepare(reference);QVERIFY(reference.deleteNotes({"3","4"}));QVERIFY(reference.setPianoLength(192));
        const auto shortened=exported(session,"short.wav",2);QVERIFY(shortened==exported(reference,"reference.wav",2));QVERIFY(shortened!=full);
        // Edits stay inside the active length; silent notes cannot be moved.
        const auto notes=session.pianoNotes();
        QVERIFY(!session.addNote(192,24,60,80));QVERIFY(session.message().contains("inside the clip"));QVERIFY(!session.moveNotes({"3"},-24,0));
        QVERIFY(!session.setPianoLength(0));QVERIFY(!session.setPianoLength(piano_length_limit+1));QCOMPARE(session.pianoNotes(),notes);QCOMPARE(session.pianoLength(),192);
        QVERIFY(session.setPianoLength(384));QVERIFY(exported(session,"again.wav",2)==full);
        // Loop off: the first bar plays, then silence.
        QVERIFY(session.setPianoLoop(false));QVERIFY(!session.pianoLoop());QCOMPARE(session.undoText(),QString("Undo clip loop"));
        const auto once=exported(session,"once.wav",2);
        QVERIFY(std::equal(once.begin(),once.begin()+2*96000,full.begin()));
        QVERIFY(std::all_of(once.begin()+2*96000,once.end(),[](float v){return v==0;}));
        // Save/Open keeps the length, the loop flag and the silent notes.
        QVERIFY(session.setPianoLength(192));
        const auto path=directory.filePath("length.json");QVERIFY(session.saveProject(path));
        Session reopened;QVERIFY(reopened.loadProject(path));
        QCOMPARE(reopened.pianoLength(),192);QVERIFY(!reopened.pianoLoop());QCOMPARE(reopened.pianoNotes(),session.pianoNotes());
        session.undo();session.undo();QCOMPARE(session.pianoLength(),384);QVERIFY(session.pianoLoop());
    }
    void qmlPianoRollLength() {
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        ui.click("pianoMode");QTRY_VERIFY(session.pianoMode());QCOMPARE(ui.item("pianoLengthSpin")->property("value").toInt(),16);
        ui.type("pianoLengthSpin","8",true);QTRY_COMPARE(session.pianoLength(),192);
        QCOMPARE(ui.item("pianoRange")->property("text").toString().contains("2 beats"),true);
        ui.click("pianoLoop");QTRY_VERIFY(!session.pianoLoop());
        ui.click("undoButton");QTRY_VERIFY(session.pianoLoop());ui.click("undoButton");QTRY_COMPARE(session.pianoLength(),384);
    }
    // #41: the densest supported clip (1024 notes, 15 sounding at once over
    // 16 bars) in the production QML. Prints timings for the receipt; asserts
    // only behavior (hit-testing, selection and edits on the dense grid).
    void pianoRollDenseWorkload() {
        QTemporaryDir directory;const auto path=directory.filePath("dense.json");
        auto project=defaultProject();
        auto& clip=*std::find_if(project.content.clips.begin(),project.content.clips.end(),[&](const auto& c){return c.clip==project.identities.clips[1].id;});
        PianoRoll roll{piano_length_limit,true,1,{}};
        for(int i=0;i<1024;++i)roll.notes.push_back({roll.next_id++,i*6,std::min(90,piano_length_limit-i*6),48+(i*7)%24,40+i%60});
        clip.piano=roll;writeAuthoredProject(path,project);
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        int frames=0;QObject::connect(ui.window,&QQuickWindow::frameSwapped,[&]{++frames;});
        const auto nextFrame=[&]{const int seen=frames;ui.window->update();return QTest::qWaitFor([&]{return frames>seen;},5000);};
        QElapsedTimer timer;
        // Baseline: the same window with the four-note template clip in the piano roll.
        ui.click("pianoMode");QTRY_VERIFY(session.pianoMode());QVERIFY(nextFrame());
        std::vector<double> baseline;
        for(int i=0;i<60;++i){timer.restart();QVERIFY(nextFrame());baseline.push_back(timer.nsecsElapsed()/1e6);}
        session.undo();QVERIFY(!session.dirty());
        timer.start();ui.type("projectFile",path);ui.click("openProjectButton");
        QTRY_COMPARE(session.pianoNotes().size(),1024);QVERIFY(nextFrame());const auto openMs=timer.nsecsElapsed()/1e6;
        QTRY_COMPARE(ui.item("pianoNotesView")->property("notes").toList().size(),1024);
        std::vector<double> frame,edit,input;
        const auto paints=[&]{return ui.item("pianoNotesView")->property("paints").toInt();};
        const int beforeIdle=paints();
        for(int i=0;i<60;++i){timer.restart();QVERIFY(nextFrame());frame.push_back(timer.nsecsElapsed()/1e6);}
        const int idleRepaints=paints()-beforeIdle;QCOMPARE(idleRepaints,0); // idle frames reuse the painted texture
        for(int i=0;i<20;++i) {
            timer.restart();QVERIFY(session.setNotesVelocity({QString::number(1+i)},100-i));QVERIFY(nextFrame());edit.push_back(timer.nsecsElapsed()/1e6);
        }
        auto* roll_=ui.item("pianoRoll");
        const auto at=[&](double tick,int pitch){
            const int base=roll_->property("base").toInt(),rows=roll_->property("rows").toInt();const double row=roll_->property("rowHeight").toDouble();
            return roll_->mapToScene(QPointF(tick*roll_->width()/session.pianoLength(),(base+rows-1-pitch+0.5)*row)).toPoint();};
        for(int i=0;i<20;++i) {
            // Note k starts at 6k with pitch 48 + 7k mod 24; aim at the middle of a note nobody else covers at that pitch.
            const int k=100+i*37;const int pitch=48+(k*7)%24;
            timer.restart();QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,at(k*6+45,pitch));
            QTRY_COMPARE(session.selectedNotes(),(QVariantList{QString::number(k+1)}));QVERIFY(nextFrame());input.push_back(timer.nsecsElapsed()/1e6);
        }
        // Edits on the dense grid keep data and selection: move the selected note and delete it.
        const auto selected=session.selectedNotes();QTest::keyClick(ui.window,Qt::Key_Right);
        QTRY_VERIFY(session.selectedNotes()==selected);QTest::keyClick(ui.window,Qt::Key_Delete);QTRY_COMPARE(session.pianoNotes().size(),1023);
        const auto stats=[](std::vector<double> v){std::sort(v.begin(),v.end());double sum=0;for(auto x:v)sum+=x;
            return QString("{\"mean\":%1,\"median\":%2,\"max\":%3}").arg(sum/v.size(),0,'f',2).arg(v[v.size()/2],0,'f',2).arg(v.back(),0,'f',2);};
        const auto api=ui.window->rendererInterface()->graphicsApi();
        std::cout<<qPrintable(QString("DENSE {\"notes\":1024,\"window\":\"%1x%2\",\"dpr\":%3,\"graphics_api\":%4,\"roll\":\"%5x%6\",\"open_ms\":%7,\"frame_ms\":%8,\"edit_to_frame_ms\":%9,\"click_to_frame_ms\":%10,\"baseline_4_notes_frame_ms\":%11,\"idle_repaints\":%12}")
            .arg(ui.window->width()).arg(ui.window->height()).arg(ui.window->devicePixelRatio()).arg(static_cast<int>(api))
            .arg(qRound(roll_->width())).arg(qRound(roll_->height())).arg(openMs,0,'f',1).arg(stats(frame)).arg(stats(edit)).arg(stats(input)).arg(stats(baseline)).arg(idleRepaints))<<std::endl;
    }
    // #39: every piano-roll operation through Undo/Redo and Save/Open, and
    // refused loads that leave the open document and its history alone.
    void pianoRollHistoryAndPersistence() {
        QTemporaryDir directory;Session session;
        struct State { QVariantList notes; int length; bool loop; bool mode; QString undo; };
        const auto state=[&]{return State{session.pianoNotes(),session.pianoLength(),session.pianoLoop(),session.pianoMode(),session.undoText()};};
        const auto same=[](const State& a,const State& b){return a.notes==b.notes && a.length==b.length && a.loop==b.loop && a.mode==b.mode && a.undo==b.undo;};
        std::vector<State> states{state()};
        const std::vector<std::function<bool()>> edits{
            [&]{return session.usePianoRoll(true);},
            [&]{return session.addNote(24,36,62,55);},
            [&]{return session.moveNotes({"5","2"},12,-3);},
            [&]{return session.resizeNotes({"5"},30);},
            [&]{return session.setNotesVelocity({"1","5"},33);},
            [&]{return session.deleteNotes({"3"});},
            [&]{return session.setPianoLength(250);},
            [&]{return session.setPianoLoop(false);},
            [&]{return session.addNote(200,49,127,1);},
        };
        for(std::size_t i=0;i<edits.size();++i) {
            QVERIFY2(edits[i](),qPrintable(QString::number(i)+": "+session.message()));
            states.push_back(state());
            // Every intermediate state survives Save and a fresh Open, IDs included.
            const auto path=directory.filePath(QString("step%1.json").arg(i));QVERIFY(session.saveProject(path));
            Session reopened;QVERIFY(reopened.loadProject(path));
            QCOMPARE(reopened.pianoNotes(),states.back().notes);QCOMPARE(reopened.pianoLength(),states.back().length);
            QCOMPARE(reopened.pianoLoop(),states.back().loop);QCOMPARE(reopened.pianoMode(),states.back().mode);
        }
        QVariantMap added,past;for(const auto& n:session.pianoNotes()){const auto m=n.toMap();if(m["id"]=="6")added=m;if(m["id"]=="4")past=m;}
        QCOMPARE(added["sounding"].toBool(),true);QCOMPARE(added["pitch"].toInt(),127);QCOMPARE(past["sounding"].toBool(),false); // 288 is past the 250-tick end
        for(std::size_t i=states.size()-1;i>0;--i){QVERIFY(same(state(),states[i]));session.undo();}
        QVERIFY(same(state(),states[0]));QVERIFY(!session.canUndo());
        for(std::size_t i=1;i<states.size();++i){session.redo();QVERIFY2(same(state(),states[i]),qPrintable(QString::number(i)));}
        QVERIFY(!session.canRedo());
        // Refused loads change nothing: the open document, its history and its path stay.
        const auto good=directory.filePath(QString("step%1.json").arg(edits.size()-1));
        QFile file(good);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();file.close();
        const auto corrupt=[&](const QString& name,auto edit){
            auto root=QJsonDocument::fromJson(bytes).object();edit(root);
            const auto path=directory.filePath(name);QFile out(path);if(!out.open(QIODevice::WriteOnly))throw std::runtime_error("fixture");out.write(QJsonDocument(root).toJson());out.close();return path;};
        const auto withNote=[](auto change){return [change](QJsonObject& root){auto clips=root["clips"].toArray();auto clip=clips[1].toObject();
            auto piano=clip["piano"].toObject();auto notes=piano["notes"].toArray();auto note=notes[0].toObject();change(note,piano);
            notes[0]=note;piano["notes"]=notes;clip["piano"]=piano;clips[1]=clip;root["clips"]=clips;};};
        const QStringList broken{
            corrupt("pitch.json",withNote([](QJsonObject& n,QJsonObject&){n["pitch"]=200;})),
            corrupt("unissued.json",withNote([](QJsonObject&,QJsonObject& p){p["next_id"]="2";})),
            corrupt("extra.json",withNote([](QJsonObject& n,QJsonObject&){n["colour"]="red";})),
            corrupt("loop.json",withNote([](QJsonObject&,QJsonObject& p){p["loop"]=true;})),
            corrupt("feature.json",[](QJsonObject& root){root["required_features"]=QJsonArray{"general-session-v1","synth-v1"};}),
            corrupt("future.json",[](QJsonObject& root){auto f=root["required_features"].toArray();f.append("piano-roll-v2");root["required_features"]=f;}),
        };
        session.undo();const auto before=state();const auto path=session.projectPath();const bool dirty=session.dirty();
        for(const auto& file:broken) {
            QVERIFY2(!session.loadProject(file),qPrintable(file));
            QVERIFY(same(state(),before));QCOMPARE(session.projectPath(),path);QCOMPARE(session.dirty(),dirty);QVERIFY(session.canRedo());
        }
        session.redo();QVERIFY(same(state(),states.back()));
    }
    // #42: notes built and edited in the QML render with the edited timing,
    // pitch and velocity, also after Undo/Redo and reopening; every
    // termination path leaves no voice sounding.
    void qmlPianoRollPlaybackJourney() {
        QTemporaryDir directory;const auto project=directory.filePath("notes.json");
        const auto level=[](int velocity){return 0.25F*(1.0F*((static_cast<float>(velocity)/100)*0.1F)*1.0F);}; // track 25 %, square ±1, envelope 1
        const auto changes=[](const std::vector<float>& s,std::size_t from,std::size_t to){
            int n=0;for(std::size_t f=from+1;f<to;++f)if((s[2*f]>0)!=(s[2*(f-1)]>0))++n;return n;};
        std::vector<float> first;
        {
            Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
            ui.click("mute0");QTRY_VERIFY(session.mutes()[0].toBool());
            ui.click("pianoMode");QTRY_VERIFY(session.pianoMode());
            auto* roll=ui.item("pianoRoll");
            const auto at=[&](double tick,int pitch){
                const int base=roll->property("base").toInt(),rows=roll->property("rows").toInt();const double row=roll->property("rowHeight").toDouble();
                return roll->mapToScene(QPointF(tick*roll->width()/session.pianoLength(),(base+rows-1-pitch+0.5)*row)).toPoint();};
            // Remove the converted notes with the pointer and the Delete key.
            ui.click("pianoOctaveUp");QTRY_COMPARE(roll->property("base").toInt(),60);
            QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,at(10,60));
            for(auto [tick,pitch]:{std::pair{110,64},{210,67},{300,72}})QTest::mouseClick(ui.window,Qt::LeftButton,Qt::ShiftModifier,at(tick,pitch));
            QTRY_COMPARE(session.selectedNotes().size(),4);QTest::keyClick(ui.window,Qt::Key_Delete);QTRY_VERIFY(session.pianoNotes().isEmpty());
            ui.click("pianoOctaveDown");
            ui.keys("synthWaveform",Qt::Key_Down,2);QTRY_COMPARE(session.synth()[0].toInt(),2);
            // A at beat 2, then two notes on one pitch overlapping from beat 3.
            const auto add=[&](const char* start,const char* length,const char* pitch,const char* velocity){
                ui.type("noteStart",start,true);ui.type("noteLength",length,true);ui.type("notePitch",pitch,true);ui.type("noteVelocity",velocity,true);ui.click("addNoteButton");};
            add("4","2","69","90");add("8","4","57","40");add("8","2","57","60");QTRY_COMPARE(session.pianoNotes().size(),3);
            // Drag A one sixteenth later.
            const auto from=at(100,69);QTest::mousePress(ui.window,Qt::LeftButton,Qt::NoModifier,from);
            const auto by=QPoint(qRound(24*roll->width()/session.pianoLength()),0);
            for(int step=1;step<=4;++step)QTest::mouseMove(ui.window,from+by*step/4);
            QTest::mouseRelease(ui.window,Qt::LeftButton,Qt::NoModifier,from+by);
            QTRY_COMPARE(session.pianoNotes()[0].toMap()["start"].toInt(),120);
            ui.keys("exportFormat",Qt::Key_Down,2);ui.exportTo(session,directory.filePath("a.wav"),"1");QTRY_VERIFY(!session.exporting());
            first=readWav(directory.filePath("a.wav")).samples;
            // Ticks become frames at 120 BPM by * 250: A 30000..42000, B 48000..72000, C 48000..60000.
            QCOMPARE(first.size(),std::size_t(2*96000));
            QCOMPARE(first[2*29999],0.0F);QCOMPARE(first[2*30000],level(90));QCOMPARE(std::abs(first[2*41999]),level(90));QCOMPARE(first[2*42000],0.0F);
            QCOMPARE(first[2*47999],0.0F);QCOMPARE(first[2*48000],0.25F*(1.0F*((40.0F/100)*0.1F)*1.0F+1.0F*((60.0F/100)*0.1F)*1.0F));
            QCOMPARE(std::abs(first[2*60000]),level(40));QCOMPARE(std::abs(first[2*71999]),level(40));QCOMPARE(first[2*72000],0.0F);
            QVERIFY(std::abs(changes(first,30000,42000)-220)<=2);  // 440 Hz: two sign changes per 109.1 frames
            QVERIFY(std::abs(changes(first,48000,72000)-220)<=2);  // 220 Hz over twice the length
            // Undo the drag, then Redo it: the render follows.
            ui.click("undoButton");QTRY_COMPARE(session.pianoNotes()[0].toMap()["start"].toInt(),96);
            ui.exportTo(session,directory.filePath("undone.wav"),"1");QTRY_VERIFY(!session.exporting());
            const auto undone=readWav(directory.filePath("undone.wav")).samples;QCOMPARE(undone[2*23999],0.0F);QCOMPARE(undone[2*24000],level(90));
            ui.click("redoButton");QTRY_COMPARE(session.pianoNotes()[0].toMap()["start"].toInt(),120);
            ui.type("projectFile",project);ui.click("saveProjectButton");QVERIFY(!session.dirty());QVERIFY(ui.window->close());
        }
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        ui.type("projectFile",project);ui.click("openProjectButton");QTRY_COMPARE(session.pianoNotes().size(),3);
        ui.keys("exportFormat",Qt::Key_Down,2);ui.exportTo(session,directory.filePath("reopened.wav"),"1");QTRY_VERIFY(!session.exporting());
        QVERIFY(readWav(directory.filePath("reopened.wav")).samples==first);
        // Termination paths while the clip plays on the preview clock.
        const auto ids=[&]{QVariantList r;for(const auto& n:session.pianoNotes())r.append(n.toMap()["id"]);return r;};
        const auto play=[&]{QVERIFY(session.launchScene(session.sceneId(),"immediate"));session.play();QTRY_VERIFY(session.running());
            QTRY_VERIFY_WITH_TIMEOUT(session.activeVoices()>0,10000);};
        play();QTest::keyClick(ui.window,Qt::Key_Period,Qt::ControlModifier);QTRY_VERIFY(!session.running());QCOMPARE(session.activeVoices(),0); // panic
        play();const auto all=ids();QVERIFY(session.deleteNotes(all));      // deletion: sounding notes finish, nothing restarts
        QTRY_VERIFY_WITH_TIMEOUT(session.activeVoices()==0,10000);QVERIFY(session.running());
        for(int i=0;i<250;++i){QTest::qWait(10);QCOMPARE(session.activeVoices(),0);} // a full loop later, still nothing
        session.undo();QTRY_VERIFY_WITH_TIMEOUT(session.activeVoices()>0,10000);    // the restored notes play again
        QVERIFY(session.setPianoLength(144));QVERIFY(session.running());              // replacement: shorter loop
        QTRY_VERIFY_WITH_TIMEOUT(session.activeVoices()==0,10000);QTRY_VERIFY_WITH_TIMEOUT(session.activeVoices()>0,10000); // A repeats, the cut notes end
        QVERIFY(session.setPianoLoop(false));                                        // one-shot: the clip ends, the track stops
        QTRY_VERIFY_WITH_TIMEOUT(session.activeClips()[1].toInt()<0,10000);QCOMPARE(session.activeVoices(),0);
        play();session.stop();QTRY_VERIFY(!session.running());QCOMPARE(session.activeVoices(),0); // stop
    }
    // #73/#74 in the Session: device chains load, follow duplicate and
    // delete, and malformed or unsupported chains leave the document alone.
    void deviceChainsInSession() {
        QTemporaryDir directory;const auto path=directory.filePath("devices.json");
        auto project=defaultProject();const auto steps=project.identities.tracks[1].id;
        const std::vector<Device> chain{{1,DeviceKind::transpose,12},{2,DeviceKind::gain,-6}};
        project.content.devices={{steps,chain,chainConnections(chain,fl_live::TrackKind::midi)}};project.content.next_device_id=3;
        writeAuthoredProject(path,project);
        Session session;QVERIFY(session.loadProject(path));
        QVERIFY(session.duplicateTrack(session.trackIds()[1].toString()));
        const auto saved=directory.filePath("saved.json");QVERIFY(session.saveProject(saved));
        auto reread=readAuthoredProject(saved);
        QCOMPARE(reread.content.devices.size(),std::size_t(2));QCOMPARE(reread.content.next_device_id,std::uint64_t(5));
        QCOMPARE(reread.content.devices[1].devices[0].id,std::uint64_t(3));QCOMPARE(reread.content.devices[1].devices[1].value,-6.0);
        QCOMPARE(reread.content.devices[1].connections,chainConnections({{3,DeviceKind::transpose,12},{4,DeviceKind::gain,-6}},fl_live::TrackKind::midi));
        QVERIFY(session.deleteTrack(session.trackIds()[1].toString()));QVERIFY(session.saveProject(saved));
        reread=readAuthoredProject(saved);QCOMPARE(reread.content.devices.size(),std::size_t(1));QCOMPARE(reread.content.devices[0].devices[0].id,std::uint64_t(3));
        session.undo();QCOMPARE(session.trackCount(),3);
        // Refused files: the open document, history and path stay.
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();file.close();
        const auto write=[&](const QString& name,const QByteArray& content){const auto p=directory.filePath(name);QFile f(p);
            if(!f.open(QIODevice::WriteOnly))throw std::runtime_error("fixture");f.write(content);f.close();return p;};
        auto cycle=QJsonDocument::fromJson(bytes).object();
        {auto d=cycle["devices"].toObject();auto t=d["tracks"].toArray();auto e=t[0].toObject();
         e["connections"]=QJsonArray{QJsonArray{"source","instrument"},QJsonArray{"instrument","output"},QJsonArray{"1","2"},QJsonArray{"2","1"}};
         t[0]=e;d["tracks"]=t;cycle["devices"]=d;}
        auto mismatch=QJsonDocument::fromJson(bytes).object();
        {auto d=mismatch["devices"].toObject();auto t=d["tracks"].toArray();auto e=t[0].toObject();auto l=e["devices"].toArray();
         auto o=l[0].toObject();o["kind"]="gain";o["value"]=0;l[0]=o;e["devices"]=l;t[0]=e;d["tracks"]=t;mismatch["devices"]=d;}
        QVERIFY(session.saveProject(directory.filePath("current.json"))); // Open refuses unsaved edits; save so only the file decides
        const auto current=session.projectPath();const auto tracks=session.trackIds();const auto history=session.undoText();
        QVERIFY(!history.isEmpty());
        for(const auto& bad:{write("cycle.json",QJsonDocument(cycle).toJson()),write("mismatch.json",QJsonDocument(mismatch).toJson())}) {
            QVERIFY(!session.loadProject(bad));QVERIFY(session.message().contains("cycle") || session.message().contains("MIDI port"));
            QCOMPARE(session.projectPath(),current);QCOMPARE(session.trackIds(),tracks);QCOMPARE(session.undoText(),history);
        }
    }
    // Device editing through the Session: chain order, stages, refusals and Undo.
    void deviceEditing() {
        Session session;const auto pulse=session.trackIds()[0].toString(),steps=session.trackIds()[1].toString();
        QVERIFY(session.addDevice(steps,"gain"));QVERIFY(session.addDevice(steps,"transpose"));QVERIFY(session.addDevice(steps,"transpose"));
        QCOMPARE(session.undoText(),QString("Undo track 2 new device"));
        const auto kinds=[&](int t){QStringList r;for(const auto& d:session.trackDevices(t))r.append(d.toMap()["kind"].toString()+d.toMap()["id"].toString());return r;};
        QCOMPARE(kinds(1),(QStringList{"transpose2","transpose3","gain1"})); // MIDI stage first, in insertion order
        QVERIFY(session.moveDevice("3",-1));QCOMPARE(kinds(1),(QStringList{"transpose3","transpose2","gain1"}));
        QVERIFY(!session.moveDevice("2",1));QVERIFY(session.message().contains("within its stage")); // would cross the instrument
        QVERIFY(!session.moveDevice("3",-1));QVERIFY(!session.moveDevice("3",2));
        QVERIFY(!session.addDevice(pulse,"transpose"));QVERIFY(session.message().contains("MIDI port"));QVERIFY(session.trackDevices(0).isEmpty());
        QVERIFY(!session.setDeviceValue("2",0.5));QVERIFY(!session.setDeviceValue("1",13));QVERIFY(!session.setDeviceValue("99",0));QVERIFY(!session.addDevice(pulse,"reverb"));
        for(int n=0;n<5;++n)QVERIFY(session.addDevice(pulse,"gain"));
        QVERIFY(session.setDeviceValue("4",12));QVERIFY(session.setDeviceValue("5",12));
        QVERIFY(!session.setDeviceValue("6",1));QVERIFY(session.message().contains("+24 dB"));
        for(int n=0;n<3;++n)QVERIFY(session.addDevice(pulse,"gain"));
        QVERIFY(!session.addDevice(pulse,"gain"));QVERIFY(session.message().contains("8 devices"));
        QVERIFY(session.removeDevice("6"));QCOMPARE(session.trackDevices(0).size(),7);
        // Removing every device returns the track to the default chain; Undo brings the same IDs back.
        const auto before=session.trackDevices(1);
        for(const auto& d:before)QVERIFY(session.removeDevice(d.toMap()["id"].toString()));
        QVERIFY(session.trackDevices(1).isEmpty());
        for(int n=0;n<3;++n)session.undo();
        QCOMPARE(session.trackDevices(1),before);
        QVERIFY(session.addDevice(steps,"gain"));QCOMPARE(session.trackDevices(1).last().toMap()["id"].toString(),QString("12")); // IDs are not reissued
    }
    // #75: a gain device scales the track by 10^(dB/20), together with the
    // level and a level lane, sample for sample, and survives Save/reopen.
    void gainDeviceOutput() {
        QTemporaryDir directory;
        const auto exported=[&](Session& s,const QString& name){const auto path=directory.filePath(name);
            if(!s.exportAudio(path,1,1,0,2,"float32"))throw std::runtime_error("export refused");
            for(int i=0;i<1000 && s.exporting();++i)QTest::qWait(10);return readWav(path).samples;};
        Session session;const auto pulse=session.trackIds()[0].toString();QVERIFY(session.setMute(session.trackIds()[1].toString(),true));
        QVERIFY(session.addDevice(pulse,"gain"));QVERIFY(session.setDeviceValue("1",-6));
        QVERIFY(session.addDevice(pulse,"gain"));QVERIFY(session.setDeviceValue("2",3));
        const float device=static_cast<float>(std::pow(10.0,-6.0/20))*static_cast<float>(std::pow(10.0,3.0/20));
        const auto source=pulseClip(120);
        const auto out=exported(session,"gain.wav");
        for(std::size_t f=0;f<96000;f+=97)QCOMPARE(out[2*f],(0.7F*device)*source.samples[f%source.samples.size()]);
        // A level lane replaces the level, not the device gain.
        QVERIFY(session.setAutomationPoint(pulse,"level",0,0));QVERIFY(session.setAutomationPoint(pulse,"level",400,100));
        const auto ramp=exported(session,"ramp.wav");
        for(std::size_t f=1;f<96000;f+=4999)QCOMPARE(ramp[2*f],(static_cast<float>(static_cast<double>(f)/96000.0)*device)*source.samples[f%source.samples.size()]);
        // The binding and the output survive Save/reopen.
        const auto path=directory.filePath("gain.json");QVERIFY(session.saveProject(path));
        Session reopened;QVERIFY(reopened.loadProject(path));QCOMPARE(reopened.trackDevices(0),session.trackDevices(0));
        QVERIFY(exported(reopened,"reopened.wav")==ramp);
    }
    // #76: a transpose device moves every note by whole semitones with the
    // same note-on and note-off frames; a note leaving 0-127 is dropped; a
    // live change leaves no voice sounding.
    void transposeDeviceOutput() {
        QTemporaryDir directory;
        const auto exported=[&](Session& s,const QString& name){const auto path=directory.filePath(name);
            if(!s.exportAudio(path,1,1,0,2,"float32"))throw std::runtime_error("export refused");
            for(int i=0;i<1000 && s.exporting();++i)QTest::qWait(10);return readWav(path).samples;};
        const auto changes=[](const std::vector<float>& s,std::size_t from,std::size_t to){int n=0;for(std::size_t f=from+1;f<to;++f)if((s[2*f]>0)!=(s[2*(f-1)]>0))++n;return n;};
        Session session;const auto steps=session.trackIds()[1].toString();QVERIFY(session.setMute(session.trackIds()[0].toString(),true));
        QVERIFY(session.setSynth("waveform",2));QVERIFY(session.usePianoRoll(true));QVERIFY(session.deleteNotes({"2","3","4"}));
        QVERIFY(session.moveNotes({"1"},0,-3)); // A3, 220 Hz, frames 0..17999
        const auto plain=exported(session,"plain.wav");
        QVERIFY(session.addDevice(steps,"transpose"));QVERIFY(session.setDeviceValue("1",12));
        const auto octave=exported(session,"octave.wav");
        QVERIFY(std::abs(changes(plain,0,18000)-165)<=2);QVERIFY(std::abs(changes(octave,0,18000)-330)<=2); // 220 Hz -> 440 Hz
        for(const auto* s:{&plain,&octave}){QVERIFY((*s)[0]!=0);QVERIFY((*s)[2*17999]!=0);QCOMPARE((*s)[2*18000],0.0F);} // same on and off
        QCOMPARE(std::abs(octave[0]),std::abs(plain[0])); // same velocity
        // Out of range: A3 + 48 + 48 = 153 is dropped at the second device, leaving silence, not a clamped note.
        QVERIFY(session.setDeviceValue("1",48));QVERIFY(session.addDevice(steps,"transpose"));QVERIFY(session.setDeviceValue("2",48));
        const auto dropped=exported(session,"dropped.wav");QVERIFY(std::all_of(dropped.begin(),dropped.end(),[](float v){return v==0;}));
        session.undo();QVERIFY(exported(session,"undone.wav")!=dropped);
        // Save/reopen keeps the binding and the sound.
        QVERIFY(session.setDeviceValue("1",12));QVERIFY(session.removeDevice("2"));
        const auto path=directory.filePath("transpose.json");QVERIFY(session.saveProject(path));
        Session reopened;QVERIFY(reopened.loadProject(path));QCOMPARE(reopened.trackDevices(1),session.trackDevices(1));
        QVERIFY(exported(reopened,"reopened.wav")==octave);
        // A live transpose change while the note sounds: the voice finishes, nothing sticks.
        QVERIFY(reopened.launchScene(reopened.sceneId(),"immediate"));reopened.play();QTRY_VERIFY_WITH_TIMEOUT(reopened.activeVoices()>0,5000);
        QVERIFY(reopened.setDeviceValue("1",-12));QVERIFY(reopened.message().contains("while playing"));
        QTRY_VERIFY_WITH_TIMEOUT(reopened.activeVoices()==0,5000);QTRY_VERIFY_WITH_TIMEOUT(reopened.activeVoices()>0,5000); // next loop at the new pitch
        reopened.stop();QTRY_VERIFY(!reopened.running());QCOMPARE(reopened.activeVoices(),0);
    }
    void qmlDeviceEditing() {
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        QCOMPARE(ui.item("deviceCount0")->property("text").toString(),QString("No devices"));
        QVERIFY(!ui.find(ui.window->contentItem(),"addTranspose0"));QVERIFY(ui.item("addTranspose1"));
        ui.click("addGain0");QTRY_COMPARE(session.trackDevices(0).size(),1);QCOMPARE(ui.item("deviceCount0")->property("text").toString(),QString("1 device"));
        ui.type("deviceValue0_0","-6",true);QTRY_COMPARE(session.trackDevices(0)[0].toMap()["value"].toDouble(),-6.0);
        ui.click("addGain0");ui.click("addTranspose1");QTRY_COMPARE(session.trackDevices(1).size(),1);
        ui.click("deviceUp0_1");QTRY_COMPARE(session.trackDevices(0)[0].toMap()["id"].toString(),QString("2"));
        ui.click("removeDevice0_0");QTRY_COMPARE(session.trackDevices(0).size(),1);
        ui.click("undoButton");QTRY_COMPARE(session.trackDevices(0).size(),2);
        ui.click("playButton");QTRY_VERIFY(session.running());QVERIFY(ui.item("addGain0")->property("enabled").toBool()); // device edits while playing
        ui.type("deviceValue1_0","7",true);QTRY_COMPARE(session.trackDevices(1)[0].toMap()["value"].toDouble(),7.0);QVERIFY(session.running());
        QVERIFY(ui.window->grabWindow().save("desktop-devices.png"));
    }
    // #77: device parameters automate like level and pan, follow device
    // removal and track duplication, and render against independent oracles.
    void deviceAutomationInSession() {
        QTemporaryDir directory;
        const auto exported=[&](Session& s,const QString& name,int bars){const auto path=directory.filePath(name);
            if(!s.exportAudio(path,1,bars,0,2,"float32"))throw std::runtime_error("export refused");
            for(int i=0;i<1000 && s.exporting();++i)QTest::qWait(10);return readWav(path).samples;};
        Session session;const auto pulse=session.trackIds()[0].toString(),steps=session.trackIds()[1].toString();
        QVERIFY(session.setMute(steps,true));QVERIFY(session.addDevice(pulse,"gain"));
        // Gain lane from -60 dB to 0 dB over bar 1: the factor is linear between 10^-3 and 1.
        QVERIFY(session.setAutomationPoint(pulse,"device:1",0,-60));QVERIFY(session.setAutomationPoint(pulse,"device:1",400,0));
        QCOMPARE(session.undoText(),QString("Undo track 1 automation"));QCOMPARE(session.automationPoints(0,"device:1").size(),2);
        const auto out=exported(session,"gain-lane.wav",2);const auto source=pulseClip(120);
        const float low=static_cast<float>(std::pow(10.0,-60.0/20));
        for(std::size_t f=0;f<192000;f+=4999) {
            const float factor=f>=96000 ? 1.0F : static_cast<float>(low+(1.0-static_cast<double>(low))*static_cast<double>(f)/96000.0);
            QCOMPARE(out[2*f],(0.7F*factor)*source.samples[f%source.samples.size()]);
        }
        QVERIFY(!session.setAutomationPoint(pulse,"device:1",800,13));QVERIFY(!session.setAutomationPoint(pulse,"device:9",0,0));
        QVERIFY(!session.setAutomationPoint(pulse,"device:x",0,0));
        // Transpose lane on the steps track: the template's notes at beats 1-4; +12 from beat 3.
        QVERIFY(session.setMute(steps,false));QVERIFY(session.setMute(pulse,true));QVERIFY(session.setSynth("waveform",2));
        QVERIFY(session.addDevice(steps,"transpose"));
        const auto plain=exported(session,"plain.wav",1);
        QVERIFY(session.setAutomationPoint(steps,"device:2",0,0));QVERIFY(session.setAutomationPoint(steps,"device:2",199,0));QVERIFY(session.setAutomationPoint(steps,"device:2",200,12));
        QVERIFY(!session.setAutomationPoint(steps,"device:2",300,0.5));
        const auto lifted=exported(session,"lifted.wav",1);
        const auto changes=[](const std::vector<float>& s,std::size_t from,std::size_t to){int n=0;for(std::size_t f=from+1;f<to;++f)if((s[2*f]>0)!=(s[2*(f-1)]>0))++n;return n;};
        for(std::size_t beat=0;beat<4;++beat) { // notes 60, 64, 67, 72 for 18000 frames each
            const auto from=beat*24000,to=from+18000;
            if(beat<2)QCOMPARE(changes(lifted,from,to),changes(plain,from,to));
            else QVERIFY(std::abs(changes(lifted,from,to)-2*changes(plain,from,to))<=2); // an octave up
            QCOMPARE(lifted[2*(to-1)]!=0,true);QCOMPARE(lifted[2*to],0.0F);
        }
        // Save/reopen keeps lanes bound to the same device IDs.
        const auto path=directory.filePath("lanes.json");QVERIFY(session.saveProject(path));
        Session reopened;QVERIFY(reopened.loadProject(path));
        QCOMPARE(reopened.automationPoints(1,"device:2"),session.automationPoints(1,"device:2"));QVERIFY(exported(reopened,"reopened.wav",1)==lifted);
        // Duplicating remaps the lane to the copy's device; removing a device removes its lane; Undo restores both.
        QVERIFY(session.duplicateTrack(steps));QCOMPARE(session.automationPoints(2,"device:3"),session.automationPoints(1,"device:2"));
        QVERIFY(session.automationPoints(2,"device:2").isEmpty());
        QVERIFY(session.removeDevice("2"));QVERIFY(session.automationPoints(1,"device:2").isEmpty());
        session.undo();QCOMPARE(session.automationPoints(1,"device:2").size(),3);
        // A second automated transpose on one track is refused.
        QVERIFY(session.addDevice(steps,"transpose"));QVERIFY(!session.setAutomationPoint(steps,"device:4",0,1));QVERIFY(session.message().contains("at most one"));
    }
    void qmlDeviceAutomation() {
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        ui.click("addGain0");QTRY_COMPARE(session.trackDevices(0).size(),1);
        ui.click("automationToggle0");ui.keys("automationParameter0",Qt::Key_Down,2);
        QCOMPARE(ui.item("automationParameter0")->property("currentText").toString(),QString("Gain 1"));
        QCOMPARE(ui.item("automationValue0")->property("from").toInt(),-60);QCOMPARE(ui.item("automationValue0")->property("to").toInt(),12);
        auto* lane=ui.item("automationLane0");
        QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,lane->mapToScene(QPointF(1,0)).toPoint()); // top row: +12 dB
        QTRY_COMPARE(session.automationPoints(0,"device:1").size(),1);QCOMPARE(session.automationPoints(0,"device:1")[0].toList()[1].toDouble(),12.0);
        QTest::mouseClick(ui.window,Qt::LeftButton,Qt::NoModifier,lane->mapToScene(QPointF(lane->width()*400/1600,lane->height()-1)).toPoint()); // bottom: -60 dB
        QTRY_COMPARE(session.automationPoints(0,"device:1").size(),2);QCOMPARE(session.automationPoints(0,"device:1")[1].toList()[1].toDouble(),-60.0);
        ui.type("automationAt0","2",true);ui.type("automationValue0","-6",true);ui.click("automationAdd0");
        QTRY_COMPARE(session.automationPoints(0,"device:1").size(),3);QCOMPARE(session.automationPoints(0,"device:1")[1].toList(),(QVariantList{200.0,-6.0}));
        ui.click("undoButton");QTRY_COMPARE(session.automationPoints(0,"device:1").size(),2);
    }
    // #78: the device journey through the QML, then a burst of device edits
    // while playing, with panic, Undo and voice cleanup.
    void qmlDeviceJourney() {
        QTemporaryDir directory;const auto project=directory.filePath("devices.json");
        const auto changes=[](const std::vector<float>& s,std::size_t from,std::size_t to){int n=0;for(std::size_t f=from+1;f<to;++f)if((s[2*f]>0)!=(s[2*(f-1)]>0))++n;return n;};
        std::vector<float> first;
        {
            Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
            ui.click("mute0");ui.keys("synthWaveform",Qt::Key_Down,2);QTRY_COMPARE(session.synth()[0].toInt(),2);
            // Transpose +12 and gain -6 dB on the steps track, then automate the gain from -60 dB to 0 over bar 1.
            ui.click("addTranspose1");ui.click("addGain1");QTRY_COMPARE(session.trackDevices(1).size(),2);
            ui.type("deviceValue1_0","12",true);ui.type("deviceValue1_1","-6",true);
            QTRY_COMPARE(session.trackDevices(1)[0].toMap()["value"].toDouble(),12.0);QTRY_COMPARE(session.trackDevices(1)[1].toMap()["value"].toDouble(),-6.0);
            ui.click("automationToggle1");ui.keys("automationParameter1",Qt::Key_Down,3);
            QCOMPARE(ui.item("automationParameter1")->property("currentText").toString(),QString("Gain 2"));
            ui.type("automationAt1","0",true);ui.type("automationValue1","-60",true);ui.click("automationAdd1");
            ui.type("automationAt1","4",true);ui.type("automationValue1","0",true);ui.click("automationAdd1");
            QTRY_COMPARE(session.automationPoints(1,"device:2").size(),2);
            // Play, edit the transpose live, undo it live.
            ui.click("clip1");ui.click("playButton");QTRY_VERIFY(session.running());QTRY_VERIFY_WITH_TIMEOUT(session.activeVoices()>0,5000);
            ui.type("deviceValue1_0","7",true);QTRY_COMPARE(session.trackDevices(1)[0].toMap()["value"].toDouble(),7.0);QVERIFY(session.running());
            ui.click("undoButton");QTRY_COMPARE(session.trackDevices(1)[0].toMap()["value"].toDouble(),12.0);QVERIFY(session.running());
            ui.click("stopButton");QTRY_VERIFY(!session.running());QTRY_COMPARE(session.activeVoices(),0);
            ui.type("projectFile",project);ui.click("saveProjectButton");QVERIFY(!session.dirty());
            ui.keys("exportFormat",Qt::Key_Down,2);ui.exportTo(session,directory.filePath("first.wav"),"2");QTRY_VERIFY(!session.exporting());
            first=readWav(directory.filePath("first.wav")).samples;QVERIFY(ui.window->close());
        }
        // Oracle: template notes 60/64/67/72 (18000 frames each, velocity 70, level 25 %) an octave up; gain
        // factor linear from 10^-3 to 1 over bar 1, then 1 (the lane replaces the -6 dB value).
        const float low=static_cast<float>(std::pow(10.0,-60.0/20));
        for(std::size_t f=0;f<192000;f+=1999) {
            const auto local=f%96000;const bool sounding=local%24000<18000;
            const float factor=f>=96000 ? 1.0F : static_cast<float>(low+(1.0-static_cast<double>(low))*static_cast<double>(f)/96000.0);
            const float level=(0.25F*factor)*(1.0F*((70.0F/100)*0.1F)*1.0F);
            QCOMPARE(std::abs(first[2*f]),sounding ? level : 0.0F);
        }
        const double base[]={261.63,329.63,392.0,523.25}; // C4 E4 G4 C5
        for(std::size_t beat=0;beat<4;++beat) {
            const auto from=96000+beat*24000;const double expected=2*base[beat]*2*18000/48000.0; // an octave up: twice the crossings
            QVERIFY2(std::abs(changes(first,from,from+18000)-expected)<=3,qPrintable(QString::number(changes(first,from,from+18000))));
        }
        Session session;QmlUi ui(session);QVERIFY(ui.window && QTest::qWaitForWindowExposed(ui.window));
        ui.type("projectFile",project);ui.click("openProjectButton");QTRY_COMPARE(session.trackDevices(1).size(),2);
        QCOMPARE(session.automationPoints(1,"device:2").size(),2);
        ui.keys("exportFormat",Qt::Key_Down,2);ui.exportTo(session,directory.filePath("reopened.wav"),"2");QTRY_VERIFY(!session.exporting());
        QVERIFY(readWav(directory.filePath("reopened.wav")).samples==first);
        // Burst: 60 device edits while playing, with a panic in the middle; nothing is left sounding.
        QVERIFY(session.launchScene(session.sceneId(),"immediate"));session.play();QTRY_VERIFY(session.running());
        const auto applied=session.playbackEdits();
        for(int i=0;i<60;++i) {
            QVERIFY(session.setDeviceValue("1",(i%25)-12));
            if(i%3==0)QVERIFY(session.setDeviceValue("2",-(i%20)));
            if(i==30){session.panic();QTRY_VERIFY(!session.running());QTRY_COMPARE(session.activeVoices(),0);session.play();QTRY_VERIFY(session.running());}
            if(i%10==0)session.tick();
        }
        for(int i=0;i<10;++i)session.tick();
        QVERIFY(session.playbackEdits()>applied);QCOMPARE(session.trackDevices(1)[0].toMap()["value"].toDouble(),static_cast<double>((59%25)-12));
        for(int i=0;i<84;++i)session.undo();   // back to the reopened state
        QCOMPARE(session.trackDevices(1)[0].toMap()["value"].toDouble(),12.0);
        session.stop();QTRY_VERIFY(!session.running());QCOMPARE(session.activeVoices(),0);
        ui.exportTo(session,directory.filePath("undone.wav"),"2");QTRY_VERIFY(!session.exporting());
        QVERIFY(readWav(directory.filePath("undone.wav")).samples==first);
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
    // examples/notes-and-devices.json is authored by this Session script and
    // saved by the app: rebuilding it must give the shipped bytes.
    void notesAndDevicesExampleIsAuthored() {
        QTemporaryDir directory;Session s;
        s.setTempo(112);
        const auto pulse=s.trackIds()[0].toString(),lead=s.trackIds()[1].toString();
        QVERIFY(s.renameTrack(pulse,"Pulse"));QVERIFY(s.renameTrack(lead,"Lead"));
        QVERIFY(s.addDevice(pulse,"gain"));QVERIFY(s.setDeviceValue("1",-3));
        QVERIFY(s.editPattern(lead));QVERIFY(s.setSynth("waveform",2));QVERIFY(s.setSynth("attack",5));QVERIFY(s.setSynth("release",120));
        QVERIFY(s.usePianoRoll(true));QVERIFY(s.deleteNotes({"1","2","3","4"}));QVERIFY(s.setPianoLength(768));
        const int melody[][4]={{0,2,69,90},{2,2,72,70},{4,4,74,85},{8,2,76,70},{10,2,74,65},{12,4,72,80},{16,4,69,90},{20,2,67,70},{22,2,69,75},{24,8,62,85}};
        for(const auto& n:melody)QVERIFY(s.addNote(n[0]*24,n[1]*24,n[2],n[3]));
        QVERIFY(s.addDevice(lead,"transpose"));QVERIFY(s.addDevice(lead,"gain"));QVERIFY(s.setDeviceValue("3",-4));
        // The lead rises a fourth on every second pass of its two-bar clip.
        for(const auto& p:{std::pair{0,0},{799,0},{800,5},{1599,5},{1600,0},{2399,0},{2400,5}})QVERIFY(s.setAutomationPoint(lead,"device:2",p.first,p.second));
        QVERIFY(s.addTrack("midi"));const auto bass=s.trackIds()[2].toString();QVERIFY(s.renameTrack(bass,"Bass"));
        QVERIFY(s.setSceneIncluded(bass,true));QVERIFY(s.editPattern(bass));QVERIFY(s.setSynth("waveform",3));QVERIFY(s.setSynth("release",60));
        QVERIFY(s.usePianoRoll(true));QVERIFY(s.setPianoLength(768));
        const int bassline[][4]={{0,6,57,85},{8,4,57,70},{16,6,55,85},{24,6,53,80}};
        for(const auto& n:bassline)QVERIFY(s.addNote(n[0]*24,n[1]*24,n[2],n[3]));
        QVERIFY(s.addDevice(bass,"transpose"));QVERIFY(s.setDeviceValue("4",-12));
        const auto path=directory.filePath("notes-and-devices.json");QVERIFY(s.saveProject(path));
        QFile built(path);QVERIFY(built.open(QIODevice::ReadOnly));const auto bytes=built.readAll();
        const auto shipped=QDir(qEnvironmentVariable("FL_LIVE_EXAMPLES_DIR")).filePath("notes-and-devices.json");
        if(qEnvironmentVariableIsSet("FL_LIVE_WRITE_EXAMPLE")){QFile out(shipped);QVERIFY(out.open(QIODevice::WriteOnly));out.write(bytes);}
        QFile file(shipped);QVERIFY2(file.open(QIODevice::ReadOnly),qPrintable(shipped));QCOMPARE(file.readAll(),bytes);
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
    // Level and pan lanes at a non-dividing tempo export the same samples
    // after Save and reopening in a fresh session, and match an independent
    // statement of the beat-to-frame rounding and interpolation.
    void automationOutputSurvivesReopen() {
        QTemporaryDir directory;const auto path=directory.filePath("lanes.json"),again=directory.filePath("lanes-again.json");
        auto project=defaultProject();project.content.tempo=137;
        const auto pulse=project.identities.tracks[0].id;
        project.content.automation={{pulse,AutomatedParameter::level,{{0,0},{150,100},{600,25},{1000,80}}},{pulse,AutomatedParameter::pan,{{0,-100},{800,100}}}};
        writeAuthoredProject(path,project);
        Session first;QVERIFY(first.loadProject(path));QVERIFY(first.setMute(first.trackIds()[1].toString(),true));QVERIFY(first.saveProject(again));
        QVERIFY(first.exportAudio(directory.filePath("a.wav"),1,3,0,2,"float32"));QTRY_VERIFY(!first.exporting());
        Session second;QVERIFY(second.loadProject(again));
        QCOMPARE(second.trackIds(),first.trackIds());QCOMPARE(second.mutes(),first.mutes());
        QCOMPARE(second.automationPoints(0,"level"),first.automationPoints(0,"level"));QCOMPARE(second.automationPoints(0,"pan"),first.automationPoints(0,"pan"));
        QVERIFY(second.exportAudio(directory.filePath("b.wav"),1,3,0,2,"float32"));QTRY_VERIFY(!second.exporting());
        const auto a=readWav(directory.filePath("a.wav")),b=readWav(directory.filePath("b.wav"));
        const std::size_t frames=(3ULL*48000*240000+137000-1)/137000; // three bars at 137 BPM, rounded up
        QCOMPARE(a.samples.size(),2*frames);QVERIFY(a.samples==b.samples);
        // frame = ceil(48000 * 60 * position / (tempo * 100)); linear between frames.
        const auto frame=[](std::uint64_t at){return (2880000ULL*at+13700-1)/13700;};
        const auto lane=[&](std::vector<std::pair<int,double>> points,std::size_t f){
            std::vector<std::pair<std::uint64_t,float>> p;for(auto [at,v]:points)p.push_back({frame(static_cast<std::uint64_t>(at)),static_cast<float>(v/100.0)});
            if(f<=p.front().first)return p.front().second;if(f>=p.back().first)return p.back().second;
            std::size_t i=0;while(p[i+1].first<=f)++i;
            const double t=static_cast<double>(f-p[i].first)/static_cast<double>(p[i+1].first-p[i].first);
            return static_cast<float>(p[i].second+(static_cast<double>(p[i+1].second)-p[i].second)*t);
        };
        QCOMPARE(frame(150),std::uint64_t(31533));QCOMPARE(frame(1000),std::uint64_t(210219));
        const auto source=pulseClip(137);
        for(std::size_t f=0;f<frames;f+=997) {
            const float gain=lane({{0,0},{150,100},{600,25},{1000,80}},f),pan=lane({{0,-100},{800,100}},f);
            const float x=source.samples[f%source.samples.size()];
            QCOMPARE(a.samples[2*f],gain*(pan>0 ? 1-pan : 1.0F)*x);QCOMPARE(a.samples[2*f+1],gain*(pan<0 ? 1+pan : 1.0F)*x);
        }
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
        session.play();session.tick(); // mixer edits stay available while playing
        QVERIFY(findItem(window->contentItem(),"pan1")->property("enabled").toBool());
        QVERIFY(findItem(window->contentItem(),"mute0")->property("enabled").toBool());
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
        // Undo of a tempo edit is not a playback edit: refused visibly, state kept.
        session.setTempo(121);session.play();QVERIFY(session.canUndo());session.undo();QCOMPARE(session.tempo(),121);
        QVERIFY(session.message().contains("Stop transport to change the tempo"));
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
        session.play();session.setGate(15,1);QCOMPARE(session.gates()[15].toInt(),1); // pattern edits apply while playing
        session.tick();session.setStepCount(1);QCOMPARE(session.stepCount(),1);
        session.undo();session.undo();QCOMPARE(session.gates()[15].toInt(),100);QCOMPARE(session.stepCount(),16);session.stop();session.tick();
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
