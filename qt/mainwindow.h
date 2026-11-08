#ifndef DM8168_SPATIAL_AUDIO_MAINWINDOW_H
#define DM8168_SPATIAL_AUDIO_MAINWINDOW_H

#include <QByteArray>
#include <QMainWindow>
#include <QString>
#include <QTime>
#include <QVector>
#include <QWidget>

#include <sys/types.h>

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;
class QPaintEvent;

class WaveformWidget : public QWidget
{
public:
    explicit WaveformWidget(const QString &title, QWidget *parent = 0);
    void setSamples(const QVector<int> &samples);
    void clearSamples();
    QSize minimumSizeHint() const;

protected:
    void paintEvent(QPaintEvent *event);

private:
    QString title_;
    QVector<int> samples_;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const QString &runtimeDirectory, QWidget *parent = 0);
    ~MainWindow();

private slots:
    void pollTelemetry();
    void resumeOutput();
    void muteOutput();
    void selectDirectionA();
    void selectDirectionB();
    void setBypass(bool enabled);

private:
    void buildInterface();
    bool ensureTelemetryOpen();
    void closeTelemetry();
    void processLine(const QByteArray &line);
    bool processStatus(const QString &line);
    bool processWave(const QString &line);
    void sendCommand(const QByteArray &command);
    void setConnectionState(bool connected, const QString &message);

    QString runtimeDirectory_;
    int telemetryFd_;
    dev_t telemetryDevice_;
    ino_t telemetryInode_;
    QByteArray readBuffer_;
    QTimer *pollTimer_;
    QTime statusAge_;
    bool connected_;

    QLabel *connectionLabel_;
    QLabel *backendValue_;
    QLabel *sourceValue_;
    QLabel *outputValue_;
    QLabel *blocksValue_;
    QLabel *framesValue_;
    QLabel *processValue_;
    QLabel *deadlineValue_;
    QLabel *dropsValue_;
    QLabel *modeValue_;
    QLabel *messageLabel_;
    QPushButton *resumeButton_;
    QPushButton *muteButton_;
    QPushButton *directionAButton_;
    QPushButton *directionBButton_;
    QCheckBox *bypassCheck_;
    WaveformWidget *inputWave_;
    WaveformWidget *leftWave_;
    WaveformWidget *rightWave_;
};

#endif
