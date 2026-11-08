#include "mainwindow.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const int kPreviewSamples = 16;
const int kMaximumBufferedBytes = 8192;
const int kReadBudgetBytes = 65536;
const int kStatusTimeoutMs = 1500;

void setCloseOnExec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0)
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

bool parseCounter(const QString &text, qulonglong *value)
{
    if (text.isEmpty())
        return false;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) < QLatin1Char('0') || text.at(i) > QLatin1Char('9'))
            return false;
    }
    bool ok = false;
    const qulonglong parsed = text.toULongLong(&ok);
    if (ok)
        *value = parsed;
    return ok;
}

bool parseFlag(const QString &text, bool *value)
{
    if (text == QLatin1String("0")) {
        *value = false;
        return true;
    }
    if (text == QLatin1String("1")) {
        *value = true;
        return true;
    }
    return false;
}

bool parseSamples(const QString &text, QVector<int> *samples)
{
    const QStringList values = text.trimmed().split(
        QLatin1Char(' '), QString::SkipEmptyParts);
    if (values.size() != kPreviewSamples)
        return false;
    QVector<int> parsed;
    parsed.reserve(kPreviewSamples);
    for (int i = 0; i < values.size(); ++i) {
        bool ok = false;
        const int sample = values.at(i).toInt(&ok);
        if (!ok || sample < -32768 || sample > 32767)
            return false;
        parsed.append(sample);
    }
    *samples = parsed;
    return true;
}

QLabel *addMetric(QGridLayout *layout, int row, int column,
                  const QString &name, QWidget *parent)
{
    QLabel *label = new QLabel(name, parent);
    label->setStyleSheet(QLatin1String("color: #5b6571;"));
    QLabel *value = new QLabel(QLatin1String("--"), parent);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(label, row, column);
    layout->addWidget(value, row, column + 1);
    return value;
}

} // namespace

WaveformWidget::WaveformWidget(const QString &title, QWidget *parent)
    : QWidget(parent), title_(title)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void WaveformWidget::setSamples(const QVector<int> &samples)
{
    samples_ = samples;
    update();
}

void WaveformWidget::clearSamples()
{
    samples_.clear();
    update();
}

QSize WaveformWidget::minimumSizeHint() const
{
    return QSize(180, 135);
}

void WaveformWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(249, 251, 253));
    painter.setPen(QColor(207, 216, 226));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    painter.setPen(QColor(32, 47, 64));
    painter.drawText(QRect(10, 6, width() - 20, 22),
                     Qt::AlignLeft | Qt::AlignVCenter, title_);

    const QRectF plot(12, 36, qMax(1, width() - 24),
                      qMax(1, height() - 60));
    painter.setPen(QPen(QColor(222, 228, 235), 1, Qt::DashLine));
    painter.drawLine(QPointF(plot.left(), plot.center().y()),
                     QPointF(plot.right(), plot.center().y()));
    painter.drawLine(plot.topLeft(), plot.topRight());
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());

    if (samples_.isEmpty()) {
        painter.setPen(QColor(108, 119, 132));
        painter.drawText(plot.toRect(), Qt::AlignCenter,
                         QLatin1String("No samples received"));
    } else {
        QPainterPath path;
        for (int i = 0; i < samples_.size(); ++i) {
            const double x = plot.left() + plot.width() * i /
                             qMax(1, samples_.size() - 1);
            const double y = plot.center().y() -
                             (samples_.at(i) / 32768.0) * plot.height() / 2.0;
            if (i == 0)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(26, 119, 156), 2));
        painter.drawPath(path);
    }
    painter.setPen(QColor(108, 119, 132));
    painter.drawText(QRect(10, height() - 22, width() - 20, 16),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QLatin1String("S16 preview / 16 samples"));
}

MainWindow::MainWindow(const QString &runtimeDirectory, QWidget *parent)
    : QMainWindow(parent), runtimeDirectory_(runtimeDirectory),
      telemetryFd_(-1), telemetryDevice_(0), telemetryInode_(0),
      pollTimer_(new QTimer(this)), connected_(false)
{
    buildInterface();
    setConnectionState(false, QLatin1String("Waiting for the audio backend."));
    connect(pollTimer_, SIGNAL(timeout()), this, SLOT(pollTelemetry()));
    pollTimer_->start(100);
    pollTelemetry();
}

MainWindow::~MainWindow()
{
    closeTelemetry();
}

void MainWindow::buildInterface()
{
    setWindowTitle(QLatin1String("DM8168 Spatial Audio"));
    resize(840, 480);
    setMinimumSize(640, 420);

    QWidget *central = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(12, 10, 12, 10);
    mainLayout->setSpacing(8);

    QHBoxLayout *header = new QHBoxLayout;
    QLabel *title = new QLabel(QLatin1String("DM8168 Spatial Audio"), central);
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);
    header->addWidget(title);
    header->addStretch();
    connectionLabel_ = new QLabel(QLatin1String("Disconnected"), central);
    header->addWidget(connectionLabel_);
    mainLayout->addLayout(header);

    QLabel *description = new QLabel(
        QLatin1String("Independent Qt client: audio and DSP stay in the backend."),
        central);
    description->setWordWrap(true);
    description->setStyleSheet(QLatin1String("color: #5b6571;"));
    mainLayout->addWidget(description);

    QGroupBox *controlGroup = new QGroupBox(QLatin1String("Output control"), central);
    QHBoxLayout *controlLayout = new QHBoxLayout(controlGroup);
    resumeButton_ = new QPushButton(QLatin1String("Resume output"), controlGroup);
    muteButton_ = new QPushButton(QLatin1String("Mute output"), controlGroup);
    directionAButton_ = new QPushButton(QLatin1String("Direction A"), controlGroup);
    directionBButton_ = new QPushButton(QLatin1String("Direction B"), controlGroup);
    directionAButton_->setCheckable(true);
    directionBButton_->setCheckable(true);
    QButtonGroup *directions = new QButtonGroup(this);
    directions->addButton(directionAButton_, 0);
    directions->addButton(directionBButton_, 1);
    directions->setExclusive(true);
    bypassCheck_ = new QCheckBox(QLatin1String("Bypass"), controlGroup);
    controlLayout->addWidget(resumeButton_);
    controlLayout->addWidget(muteButton_);
    controlLayout->addStretch();
    controlLayout->addWidget(directionAButton_);
    controlLayout->addWidget(directionBButton_);
    controlLayout->addWidget(bypassCheck_);
    mainLayout->addWidget(controlGroup);

    connect(resumeButton_, SIGNAL(clicked()), this, SLOT(resumeOutput()));
    connect(muteButton_, SIGNAL(clicked()), this, SLOT(muteOutput()));
    connect(directionAButton_, SIGNAL(clicked()), this, SLOT(selectDirectionA()));
    connect(directionBButton_, SIGNAL(clicked()), this, SLOT(selectDirectionB()));
    // clicked() only reflects user input; telemetry setChecked() must not echo.
    connect(bypassCheck_, SIGNAL(clicked(bool)), this, SLOT(setBypass(bool)));

    QGroupBox *statusGroup = new QGroupBox(QLatin1String("Backend status"), central);
    QGridLayout *metrics = new QGridLayout(statusGroup);
    metrics->setHorizontalSpacing(10);
    metrics->setVerticalSpacing(5);
    backendValue_ = addMetric(metrics, 0, 0, QLatin1String("Backend"), statusGroup);
    sourceValue_ = addMetric(metrics, 0, 2, QLatin1String("Filter bank"), statusGroup);
    outputValue_ = addMetric(metrics, 0, 4, QLatin1String("Output"), statusGroup);
    blocksValue_ = addMetric(metrics, 1, 0, QLatin1String("Blocks"), statusGroup);
    framesValue_ = addMetric(metrics, 1, 2, QLatin1String("Frames"), statusGroup);
    processValue_ = addMetric(metrics, 1, 4, QLatin1String("Block time"), statusGroup);
    deadlineValue_ = addMetric(metrics, 2, 0, QLatin1String("Deadline misses"), statusGroup);
    dropsValue_ = addMetric(metrics, 2, 2, QLatin1String("Preview drops"), statusGroup);
    modeValue_ = addMetric(metrics, 2, 4, QLatin1String("Mode"), statusGroup);
    metrics->setColumnStretch(1, 1);
    metrics->setColumnStretch(3, 1);
    metrics->setColumnStretch(5, 1);
    mainLayout->addWidget(statusGroup);

    QHBoxLayout *waveforms = new QHBoxLayout;
    inputWave_ = new WaveformWidget(QLatin1String("Input preview"), central);
    leftWave_ = new WaveformWidget(QLatin1String("Output left"), central);
    rightWave_ = new WaveformWidget(QLatin1String("Output right"), central);
    waveforms->addWidget(inputWave_, 1);
    waveforms->addWidget(leftWave_, 1);
    waveforms->addWidget(rightWave_, 1);
    mainLayout->addLayout(waveforms, 1);

    QLabel *hint = new QLabel(
        QLatin1String("Mute keeps the stream open. End the launcher to release ALSA devices."),
        central);
    hint->setWordWrap(true);
    hint->setStyleSheet(QLatin1String("color: #5b6571;"));
    mainLayout->addWidget(hint);
    messageLabel_ = new QLabel(central);
    messageLabel_->setWordWrap(true);
    messageLabel_->setMinimumHeight(28);
    mainLayout->addWidget(messageLabel_);
    setCentralWidget(central);
}

bool MainWindow::ensureTelemetryOpen()
{
    const QByteArray path = QFile::encodeName(
        QDir(runtimeDirectory_).filePath(QLatin1String("telemetry.fifo")));
    struct stat endpoint;
    if (::lstat(path.constData(), &endpoint) != 0 || !S_ISFIFO(endpoint.st_mode)) {
        closeTelemetry();
        return false;
    }
    if (telemetryFd_ >= 0 &&
        (endpoint.st_dev != telemetryDevice_ || endpoint.st_ino != telemetryInode_)) {
        closeTelemetry();
        setConnectionState(false, QLatin1String("Backend restarted; reconnecting."));
    }
    if (telemetryFd_ < 0) {
        telemetryFd_ = ::open(path.constData(), O_RDONLY | O_NONBLOCK);
        if (telemetryFd_ < 0)
            return false;
        setCloseOnExec(telemetryFd_);
        telemetryDevice_ = endpoint.st_dev;
        telemetryInode_ = endpoint.st_ino;
    }
    return true;
}

void MainWindow::closeTelemetry()
{
    if (telemetryFd_ >= 0)
        ::close(telemetryFd_);
    telemetryFd_ = -1;
    readBuffer_.clear();
}

void MainWindow::pollTelemetry()
{
    if (!ensureTelemetryOpen()) {
        setConnectionState(false, QLatin1String("Waiting for the audio backend FIFOs."));
        return;
    }

    int remaining = kReadBudgetBytes;
    while (remaining > 0) {
        char buffer[1024];
        const ssize_t count = ::read(telemetryFd_, buffer, sizeof(buffer));
        if (count > 0) {
            remaining -= static_cast<int>(count);
            readBuffer_.append(buffer, static_cast<int>(count));
            int newline;
            while ((newline = readBuffer_.indexOf('\n')) >= 0) {
                const QByteArray line = readBuffer_.left(newline);
                readBuffer_.remove(0, newline + 1);
                if (line.size() <= kMaximumBufferedBytes)
                    processLine(line);
            }
            if (readBuffer_.size() > kMaximumBufferedBytes)
                readBuffer_.clear();
        } else if (count == 0) {
            // Keep the reader open so a new nonblocking writer can attach.
            // A writer's incomplete final line must not join a later session.
            readBuffer_.clear();
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            closeTelemetry();
            setConnectionState(false, QLatin1String("Telemetry disconnected; retrying."));
            break;
        }
    }

    if (connected_) {
        const int age = statusAge_.elapsed();
        if (age < 0 || age > kStatusTimeoutMs)
            setConnectionState(false, QLatin1String("No recent backend status; waiting to reconnect."));
    }
}

void MainWindow::processLine(const QByteArray &line)
{
    const QString text = QString::fromUtf8(line.constData(), line.size()).trimmed();
    if (text.startsWith(QLatin1String("STATUS ")))
        processStatus(text);
    else if (text.startsWith(QLatin1String("WAVE ")) && connected_)
        processWave(text);
}

bool MainWindow::processStatus(const QString &line)
{
    const QStringList tokens = line.mid(7).split(
        QLatin1Char(' '), QString::SkipEmptyParts);
    QMap<QString, QString> fields;
    for (int i = 0; i < tokens.size(); ++i) {
        const int equals = tokens.at(i).indexOf(QLatin1Char('='));
        if (equals <= 0)
            return false;
        const QString key = tokens.at(i).left(equals);
        if (fields.contains(key))
            return false;
        fields.insert(key, tokens.at(i).mid(equals + 1));
    }

    qulonglong blocks, frames, deadlineMisses, visualDrops;
    bool running, directionB, bypass, synthetic;
    bool processOk = false;
    const double processUs = fields.value(QLatin1String("process_us")).toDouble(&processOk);
    const QString backend = fields.value(QLatin1String("backend"));
    if (!parseCounter(fields.value(QLatin1String("blocks")), &blocks) ||
        !parseCounter(fields.value(QLatin1String("frames")), &frames) ||
        !parseCounter(fields.value(QLatin1String("deadline_misses")), &deadlineMisses) ||
        !parseCounter(fields.value(QLatin1String("visual_drops")), &visualDrops) ||
        !parseFlag(fields.value(QLatin1String("running")), &running) ||
        !parseFlag(fields.value(QLatin1String("direction")), &directionB) ||
        !parseFlag(fields.value(QLatin1String("bypass")), &bypass) ||
        !parseFlag(fields.value(QLatin1String("synthetic")), &synthetic) ||
        !processOk || !(processUs >= 0.0 && processUs <= 1.0e12) ||
        (backend != QLatin1String("cpu") && backend != QLatin1String("rpe"))) {
        return false;
    }

    statusAge_.start();
    setConnectionState(true,
        QLatin1String("Live backend telemetry. Preview is not a full audio waveform."));
    backendValue_->setText(backend == QLatin1String("rpe")
        ? QLatin1String("RPE / DSP") : QLatin1String("CPU reference"));
    sourceValue_->setText(synthetic ? QLatin1String("Synthetic test")
                                    : QLatin1String("Imported data"));
    sourceValue_->setStyleSheet(synthetic ? QLatin1String("color: #9a5c00;")
                                         : QLatin1String("color: #203040;"));
    outputValue_->setText(running ? QLatin1String("Running") : QLatin1String("Muted"));
    blocksValue_->setText(QString::number(blocks));
    framesValue_->setText(QString::number(frames));
    processValue_->setText(QString::number(processUs, 'f', 1) + QLatin1String(" us"));
    deadlineValue_->setText(QString::number(deadlineMisses));
    dropsValue_->setText(QString::number(visualDrops));
    modeValue_->setText(QString(directionB ? QLatin1String("B") : QLatin1String("A")) +
                        (bypass ? QLatin1String(" / bypass") : QLatin1String(" / FIR")));
    directionAButton_->setChecked(!directionB);
    directionBButton_->setChecked(directionB);
    bypassCheck_->setChecked(bypass);
    resumeButton_->setEnabled(!running);
    muteButton_->setEnabled(running);
    return true;
}

bool MainWindow::processWave(const QString &line)
{
    const QStringList channels = line.mid(5).split(QLatin1Char('|'));
    if (channels.size() != 3)
        return false;
    QVector<int> input, left, right;
    if (!parseSamples(channels.at(0), &input) ||
        !parseSamples(channels.at(1), &left) ||
        !parseSamples(channels.at(2), &right)) {
        return false;
    }
    inputWave_->setSamples(input);
    leftWave_->setSamples(left);
    rightWave_->setSamples(right);
    return true;
}

void MainWindow::sendCommand(const QByteArray &command)
{
    if (!connected_) {
        messageLabel_->setText(QLatin1String("Command not sent: backend is disconnected."));
        return;
    }
    const QByteArray path = QFile::encodeName(
        QDir(runtimeDirectory_).filePath(QLatin1String("control.fifo")));
    struct stat endpoint;
    if (::lstat(path.constData(), &endpoint) != 0 || !S_ISFIFO(endpoint.st_mode)) {
        setConnectionState(false, QLatin1String("Control FIFO unavailable; waiting to reconnect."));
        return;
    }
    const int fd = ::open(path.constData(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        setConnectionState(false, QLatin1String("Control reader unavailable; command not sent."));
        return;
    }
    setCloseOnExec(fd);
    ssize_t written;
    do {
        written = ::write(fd, command.constData(), static_cast<size_t>(command.size()));
    } while (written < 0 && errno == EINTR);
    const int writeError = errno;
    ::close(fd);
    if (written == command.size()) {
        messageLabel_->setText(QLatin1String("Command sent; waiting for backend status."));
    } else if (written < 0 && (writeError == EAGAIN || writeError == EWOULDBLOCK)) {
        messageLabel_->setText(QLatin1String("Control channel busy; command not sent. Retry when ready."));
    } else {
        setConnectionState(false, QLatin1String("Control disconnected; command not sent."));
    }
}

void MainWindow::setConnectionState(bool connected, const QString &message)
{
    const bool wasConnected = connected_;
    connected_ = connected;
    connectionLabel_->setText(connected ? QLatin1String("Connected")
                                        : QLatin1String("Disconnected"));
    connectionLabel_->setStyleSheet(connected ? QLatin1String("color: #18734c; font-weight: bold;")
                                              : QLatin1String("color: #a34423; font-weight: bold;"));
    resumeButton_->setEnabled(connected);
    muteButton_->setEnabled(connected);
    directionAButton_->setEnabled(connected);
    directionBButton_->setEnabled(connected);
    bypassCheck_->setEnabled(connected);
    messageLabel_->setText(message);
    if (!connected && (wasConnected || blocksValue_->text() != QLatin1String("--"))) {
        backendValue_->setText(QLatin1String("--"));
        sourceValue_->setText(QLatin1String("--"));
        outputValue_->setText(QLatin1String("--"));
        blocksValue_->setText(QLatin1String("--"));
        framesValue_->setText(QLatin1String("--"));
        processValue_->setText(QLatin1String("--"));
        deadlineValue_->setText(QLatin1String("--"));
        dropsValue_->setText(QLatin1String("--"));
        modeValue_->setText(QLatin1String("--"));
        inputWave_->clearSamples();
        leftWave_->clearSamples();
        rightWave_->clearSamples();
    }
}

void MainWindow::resumeOutput()
{
    sendCommand(QByteArray("START\n"));
}

void MainWindow::muteOutput()
{
    sendCommand(QByteArray("STOP\n"));
}

void MainWindow::selectDirectionA()
{
    sendCommand(QByteArray("DIRECTION 0\n"));
}

void MainWindow::selectDirectionB()
{
    sendCommand(QByteArray("DIRECTION 1\n"));
}

void MainWindow::setBypass(bool enabled)
{
    sendCommand(enabled ? QByteArray("BYPASS 1\n") : QByteArray("BYPASS 0\n"));
}
