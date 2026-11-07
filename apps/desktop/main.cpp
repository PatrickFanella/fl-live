#include "session.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QDebug>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("fl-live");
    QCoreApplication::setApplicationVersion(QStringLiteral(FL_LIVE_VERSION " (source " FL_LIVE_SOURCE_REVISION ")"));
    QCommandLineParser parser;
    parser.setApplicationDescription("Linux session prototype. Defaults to silent preview.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"remote", "Explicit PipeWire remote", "name"});
    parser.addOption({"target", "Explicit output target (requires remote)", "node"});
    parser.addOption({"input", "Explicit recording input node (requires remote)", "node"});
    parser.process(app);
    if ((parser.isSet("target") || parser.isSet("input")) && !parser.isSet("remote")) parser.showHelp(1);
    try {
        fl_live::PipeWireConfig config;
        config.remote = parser.value("remote").toStdString();
        config.target = parser.value("target").toStdString();
        config.autoconnect = !config.target.empty();
        config.input_target = parser.value("input").toStdString();
        Session session(config);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("session", &session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        if (engine.rootObjects().isEmpty()) return 1;
        return app.exec();
    } catch (const std::exception& error) { qCritical() << error.what(); return 1; }
}
