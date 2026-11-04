#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

namespace crate {

class AddServerFlow;
class CrateServers;

/// The steps of adding a Crate server (AddServerFlow) as widgets: address,
/// confirmation for plain HTTP on the local network, sign-in. Used by the
/// Crate preferences page and the first-run dialog.
class AddServerWidget : public QWidget {
    Q_OBJECT
  public:
    explicit AddServerWidget(CrateServers* pServers, QWidget* parent = nullptr);

    /// Forgets a server being added and starts again from the address.
    void reset();

  signals:
    void serverAdded(const QString& serverId);
    void cancelled();

  private:
    void showStep();

    AddServerFlow* m_pFlow;
    QStackedWidget* m_pSteps;
    QLineEdit* m_pAddress;
    QLabel* m_pLocalNetworkWarning;
    QLineEdit* m_pEmail;
    QLineEdit* m_pPassword;
    QLabel* m_pError;
};

} // namespace crate
