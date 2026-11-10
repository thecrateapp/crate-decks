#pragma once

#include <QString>
#include <QWidget>

#include "crate/crateaddserverflow.h"

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

namespace crate {

class CrateServers;

/// The steps of adding a Crate server (AddServerFlow) as widgets: address,
/// confirmation for plain HTTP on the local network, sign-in. Used by the
/// Crate preferences page and the first-run dialog.
///
/// Laid out as the GU1 first-run mockup (plan V2.PR5): a MonoLabel over each
/// field, the address field with the server icon and the HTTPS helper, and
/// one row of actions under the steps: the step's own (Connect, Connect
/// without encryption, Sign in...) and Cancel.
class AddServerWidget : public QWidget {
    Q_OBJECT
  public:
    explicit AddServerWidget(CrateServers* pServers, QWidget* parent = nullptr);

    /// Forgets a server being added and starts again from the address.
    void reset();

    /// The text of the cancel action ("Not now" in the first-run dialog).
    void setCancelText(const QString& text);
    /// Puts `pWidget` at the end of the row of actions, on the right.
    void addTrailing(QWidget* pWidget);

    /// The step of adding a server as the first-run mockup numbers them:
    /// 1 address, 2 check (and the confirmation of plain HTTP), 3 sign in.
    static int stepNumber(AddServerFlow::Step step);
    static constexpr int kStepCount = 3;

  signals:
    void serverAdded(const QString& serverId);
    void cancelled();
    /// The step of the flow changed: its stepNumber().
    void stepNumberChanged(int number);

  private:
    void showStep();

    AddServerFlow* m_pFlow;
    QStackedWidget* m_pSteps;
    QLineEdit* m_pAddress;
    QLabel* m_pLocalNetworkWarning;
    QLineEdit* m_pEmail;
    QLineEdit* m_pPassword;
    QLabel* m_pError;
    QHBoxLayout* m_pActions;
    QPushButton* m_pConnect;
    QPushButton* m_pConnectAnyway;
    QPushButton* m_pSignIn;
    QPushButton* m_pGoogle;
    QPushButton* m_pApple;
    QPushButton* m_pCancel;
};

} // namespace crate
