#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QPixmap>
#include <QString>
#include <QTimer>

#include <signal.h>
#include <stdio.h>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("DM8168 Spatial Audio");
    app.setOrganizationName("DM8168 Audio Lab");

    // A backend can close its FIFO between open() and write().
    // Convert SIGPIPE into an ordinary EPIPE error handled by the UI.
    ::signal(SIGPIPE, SIG_IGN);

    QString runtimeDirectory = QLatin1String("/tmp/dm8168-spatial-audio");
    QString screenshotPath;
    int quitAfterMs = -1;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QLatin1String("--help") ||
            argument == QLatin1String("-h")) {
            ::printf("Usage: dm8168-audio-qt [--runtime-dir DIRECTORY] [Qt options]\n"
                     "       [--screenshot PATH.png] [--quit-after-ms N]\n"
                     "The backend must create control.fifo and telemetry.fifo.\n"
                     "This client does not start audio capture or DSP processing.\n");
            return 0;
        }
        if (argument == QLatin1String("--runtime-dir")) {
            if (i + 1 >= argc) {
                ::fprintf(stderr, "--runtime-dir requires a directory argument\n");
                return 2;
            }
            runtimeDirectory = QString::fromLocal8Bit(argv[++i]);
            if (runtimeDirectory.isEmpty()) {
                ::fprintf(stderr, "--runtime-dir must not be empty\n");
                return 2;
            }
        } else if (argument == QLatin1String("--screenshot")) {
            if (i + 1 >= argc) {
                ::fprintf(stderr, "--screenshot requires a PNG path\n");
                return 2;
            }
            screenshotPath = QString::fromLocal8Bit(argv[++i]);
            if (screenshotPath.isEmpty()) {
                ::fprintf(stderr, "--screenshot must not be empty\n");
                return 2;
            }
        } else if (argument == QLatin1String("--quit-after-ms")) {
            if (i + 1 >= argc) {
                ::fprintf(stderr, "--quit-after-ms requires a nonnegative integer\n");
                return 2;
            }
            bool valid = false;
            quitAfterMs = QString::fromLocal8Bit(argv[++i]).toInt(&valid);
            if (!valid || quitAfterMs < 0) {
                ::fprintf(stderr, "--quit-after-ms requires a nonnegative integer\n");
                return 2;
            }
        } else {
            ::fprintf(stderr, "Unknown argument; use --help for usage\n");
            return 2;
        }
    }

    MainWindow window(QDir::cleanPath(runtimeDirectory));
    window.show();
    app.processEvents();
    if (!screenshotPath.isEmpty()) {
#if QT_VERSION >= 0x050000
        const QPixmap screenshot = window.grab();
#else
        const QPixmap screenshot = QPixmap::grabWidget(&window);
#endif
        if (screenshot.isNull() || !screenshot.save(screenshotPath, "PNG")) {
            ::fprintf(stderr, "Cannot save screenshot: %s\n",
                      screenshotPath.toLocal8Bit().constData());
            return 3;
        }
    }
    if (quitAfterMs >= 0) {
        QTimer::singleShot(quitAfterMs, &app, SLOT(quit()));
    }
    return app.exec();
}
