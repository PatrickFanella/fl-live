#include "session.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <cstdlib>
#include <QSaveFile>
#include <iostream>
#include <stdexcept>

// Driven by tests/atomic_save_faults.py, normally under LD_PRELOAD of the
// fault injector. Modes:
//   faults DIR  - real Session saves fail at write, sync and rename
//   loop DIR    - alternate two large authored projects until killed
//   verify DIR  - the destination opens after interrupted saves
namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
QByteArray bytes(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "cannot read " + path.toStdString());
    return file.readAll();
}
QStringList entries(const QString& dir) {
    return QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}
AuthoredProject large(int tempo) {
    // Many named scenes make the file large enough (about 90 KB) that a kill
    // can land inside the write, not only between saves.
    auto project = defaultProject();
    project.content.tempo = tempo;
    fl_live::SessionDocument document(project.identities);
    const auto source = project.identities.scenes[0];
    for (int i = 1; i < 256; ++i) {
        const auto scene = document.createScene(std::string(120, static_cast<char>('a' + i % 26)) + std::to_string(i));
        for (const auto& entry : source.entries) document.setSlot(scene, entry.track, entry.clip);
    }
    project.identities = document.snapshot();
    return project;
}
int faults(const QString& dir) {
    const auto path = QDir(dir).filePath("project.json");
    const auto neighbor = QDir(dir).filePath("neighbor.bin");
    {
        QFile file(neighbor);
        require(file.open(QIODevice::WriteOnly) && file.write(QByteArray(4096, '\x5a')) == 4096, "neighbor");
    }
    Session session;
    session.setTempo(130);
    unsetenv("FL_LIVE_FAULT");
    require(session.saveProject(path) && !session.dirty(), "baseline save failed");
    const auto baseline = bytes(path);
    const auto listing = entries(dir);
    session.setTempo(140);
    session.setPitch(0, 50);
    const auto undo = session.undoText();
    for (const char* fault : {"write", "sync", "rename"}) {
        setenv("FL_LIVE_FAULT", fault, 1);
        const bool saved = session.saveProject(path);
        const bool savedAs = session.saveProject(QDir(dir).filePath("new.json"));
        unsetenv("FL_LIVE_FAULT");
        const std::string name(fault);
        require(!saved && !savedAs, name + ": faulted save reported success");
        require(session.message().contains("previous file was preserved"), name + ": message " + session.message().toStdString());
        require(bytes(path) == baseline, name + ": destination bytes changed");
        require(bytes(neighbor) == QByteArray(4096, '\x5a'), name + ": unrelated file changed");
        require(entries(dir) == listing, name + ": temporary or new file left behind");
        require(session.dirty() && session.projectPath() == path && session.tempo() == 140 &&
                session.pitches()[0].toInt() == 50 && session.undoText() == undo && session.canUndo(), name + ": session state changed");
        // The open project still reloads after the failed save.
        require(readAuthoredProject(path).content.tempo == 130, name + ": destination no longer readable");
        std::cout << "PASS fault " << fault << '\n';
    }
    // History still works and a later save succeeds.
    session.undo();
    require(session.pitches()[0].toInt() == 60 && session.dirty(), "undo after failed saves");
    session.redo();
    require(session.saveProject(path) && !session.dirty(), "save after faults failed");
    const auto saved = readAuthoredProject(path);
    require(saved.content.tempo == 140 && templateView(saved.content, saved.identities).steps[0].pitch == 50, "saved content");
    std::cout << "PASS: write, sync and rename faults preserve destination, neighbors, document, history and dirty state" << std::endl;
    return 0;
}
int loop(const QString& dir) {
    const auto path = QDir(dir).filePath("project.json");
    const AuthoredProject a = large(121), b = large(173);
    for (const auto& [name, project] : {std::pair{"a.ref", &a}, std::pair{"b.ref", &b}}) {
        QFile file(QDir(dir).filePath(QString::fromLatin1(name)));
        const auto encoded = encodeAuthoredProject(*project);
        require(file.open(QIODevice::WriteOnly) && file.write(encoded) == encoded.size(), "reference");
    }
    writeAuthoredProject(path, a);
    std::cout << "{\"ready\":true}" << std::endl;
    for (std::uint64_t n = 1;; ++n) writeAuthoredProject(path, n % 2 ? b : a);
}
// Leaves an autosave snapshot behind by aborting like a crash would.
int crash(const QString& dir) {
    Session session;
    require(session.saveProject(QDir(dir).filePath("crashed.json")), "initial save");
    session.setTempo(141);
    require(session.addTrack("midi"), "edit");
    require(session.autosaveNow(), "autosave");
    std::cout << session.autosaveFile().toStdString() << std::endl;
    std::abort();
}
int verify(const QString& dir) {
    const auto path = QDir(dir).filePath("project.json");
    Session session;
    require(session.loadProject(path), "interrupted destination does not open: " + session.message().toStdString());
    require(session.tempo() == 121 || session.tempo() == 173, "unexpected tempo");
    require(session.sceneIds().size() == 256, "scene count");
    std::cout << "PASS: destination opens with " << entries(dir).size() << " directory entries" << std::endl;
    return 0;
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3) return 2;
    try {
        const std::string mode = argv[1];
        const auto dir = QString::fromLocal8Bit(argv[2]);
        if (mode == "faults") return faults(dir);
        if (mode == "loop") return loop(dir);
        if (mode == "verify") return verify(dir);
        if (mode == "crash") return crash(dir);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
