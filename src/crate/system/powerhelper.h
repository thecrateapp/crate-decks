#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QProcess;

namespace crate::system {

/// The power limit helper of the hybrid laptop plan (§4): the crate-power-limit
/// service that the Linux packages ship in res/crate-power-limit without
/// enabling it. The system check offers it; install() runs its installer
/// with pkexec, which asks for the administrator password, and only after
/// the user accepted (Diego, 2026-11-15: never silently).
class PowerHelper : public QObject {
    Q_OBJECT
  public:
    explicit PowerHelper(QObject* pParent = nullptr);
    ~PowerHelper() override;

    /// The helper's files in this installation (or in the source tree).
    static QString defaultDirectory();

    /// Where the helper's files are; defaultDirectory() unless set (tests).
    void setDirectory(const QString& directory);
    QString directory() const {
        return m_directory;
    }
    /// The program that runs the installer as root: "pkexec" (tests set
    /// their own).
    void setProgram(const QString& program);

    /// Whether install() can run: Linux, the installer is there and the
    /// program is found.
    bool isAvailable() const;
    bool isRunning() const;

    /// Runs `program <directory>/install.sh` and reports with finished().
    /// Does nothing while it runs.
    void install();

  signals:
    /// The installer ended: `ok` when it exited with 0; `output` is what it
    /// printed.
    void finished(bool ok, const QString& output);

  private:
    QString m_directory;
    QString m_program;
    QPointer<QProcess> m_pProcess;
};

} // namespace crate::system
