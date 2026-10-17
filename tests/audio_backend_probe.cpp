#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include "platform/audio/AudioBackend.h"

// Explicitly invoked read-only diagnostic. It never writes mixer state, creates
// a stream, captures audio, or plays a sample. The backend disables libpulse
// autospawn; connecting an existing service socket may still activate its service.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    PactlAudioBackend backend;
    QObject::connect(&backend, &AudioBackend::readFinished, &app,
        [&app](quint64, bool ok, const AudioSnapshot &snapshot, const QString &error) {
        QJsonObject report{{"readOnly", true}, {"available", ok},
            {"serverName", snapshot.serverName}, {"serverVersion", snapshot.serverVersion},
            {"cookiePresent", !snapshot.serverCookie.isEmpty()},
            {"sinks", int(snapshot.sinks.size())}, {"sources", int(snapshot.sources.size())},
            {"cards", int(snapshot.cards.size())}, {"playbackStreams", int(snapshot.sinkInputs.size())},
            {"recordingStreams", int(snapshot.sourceOutputs.size())}, {"error", error}};
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
        app.exit(ok ? 0 : 1);
    });
    QTimer::singleShot(20000, &app, [&app] {
        QTextStream(stderr) << "Read-only audio probe timed out.\n"; app.exit(2);
    });
    backend.readCurrent(1);
    return app.exec();
}
