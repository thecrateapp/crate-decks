#include "crate/system/powerhelper.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include "moc_powerhelper.cpp"
#include "preferences/configobject.h"
#include "util/logger.h"

namespace crate::system {

namespace {

const mixxx::Logger kLogger("crate::system::PowerHelper");

QString installer(const QString& directory) {
    return QDir(directory).filePath(QStringLiteral("install.sh"));
}

} // namespace

PowerHelper::PowerHelper(QObject* pParent)
        : QObject(pParent),
          m_directory(defaultDirectory()),
          m_program(QStringLiteral("pkexec")) {
}

PowerHelper::~PowerHelper() {
    if (m_pProcess) {
        // The installer finishes on its own; nothing waits for it
        m_pProcess->disconnect(this);
    }
}

// static
QString PowerHelper::defaultDirectory() {
    return QDir(ConfigObject<ConfigValue>::computeResourcePath())
            .filePath(QStringLiteral("crate-power-limit"));
}

void PowerHelper::setDirectory(const QString& directory) {
    m_directory = directory;
}

void PowerHelper::setProgram(const QString& program) {
    m_program = program;
}

bool PowerHelper::isAvailable() const {
#ifdef Q_OS_LINUX
    const QFileInfo script(installer(m_directory));
    return script.isFile() && script.isExecutable() &&
            !QStandardPaths::findExecutable(m_program).isEmpty();
#else
    return false;
#endif
}

bool PowerHelper::isRunning() const {
    return m_pProcess != nullptr;
}

void PowerHelper::install() {
    if (isRunning()) {
        return;
    }
    auto* pProcess = new QProcess(this);
    m_pProcess = pProcess;
    pProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(pProcess,
            &QProcess::finished,
            this,
            [this, pProcess](int exitCode, QProcess::ExitStatus status) {
                const QString output = QString::fromLocal8Bit(pProcess->readAll()).trimmed();
                const bool ok = status == QProcess::NormalExit && exitCode == 0;
                kLogger.info() << "Power limit helper installer:" << exitCode << output;
                pProcess->deleteLater();
                m_pProcess = nullptr;
                emit finished(ok, output);
            });
    connect(pProcess,
            &QProcess::errorOccurred,
            this,
            [this, pProcess](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) {
                    return;
                }
                pProcess->deleteLater();
                m_pProcess = nullptr;
                emit finished(false, pProcess->errorString());
            });
    kLogger.info() << "Installing the power limit helper with" << m_program;
    pProcess->start(m_program, {installer(m_directory)});
}

} // namespace crate::system
