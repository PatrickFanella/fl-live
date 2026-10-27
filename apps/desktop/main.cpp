#include "session.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QDebug>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("fl-live");
    QCommandLineParser parser;
    parser.setApplicationDescription("Linux session prototype. Defaults to silent preview.");
    parser.addHelpOption();
    parser.addOption({"remote", "Explicit PipeWire remote", "name"});
    parser.addOption({"target", "Explicit output target (requires remote)", "node"});
    parser.process(app);
    if (parser.isSet("target") && !parser.isSet("remote")) parser.showHelp(1);
    try {
        fl_live::PipeWireConfig config;
        config.remote = parser.value("remote").toStdString();
        config.target = parser.value("target").toStdString();
        config.autoconnect = !config.target.empty();
        Session session(config);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("session", &session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        if (engine.rootObjects().isEmpty()) return 1;
        return app.exec();
    } catch (const std::exception& error) { qCritical() << error.what(); return 1; }
}
