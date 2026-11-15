#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace crate {

class CratePreferences;

namespace system {
class PowerHelper;
class SystemCheck;
} // namespace system

/// The results of a system::SystemCheck as rows (the server rows of the
/// Crate page): a dot in the result's tone, the title, what was measured,
/// the advice and, when the result offers it, its action:
/// - "Draw with the discrete GPU" turns the preference on in `pPreferences`
///   (it applies at the next start);
/// - "Install the helper" asks first, in the row, and then runs the power
///   limit helper's installer (pkexec); once installed, the check runs again.
/// While the check measures, a line says how long it has left.
class SystemCheckView : public QWidget {
    Q_OBJECT
  public:
    SystemCheckView(system::SystemCheck* pCheck,
            CratePreferences* pPreferences,
            system::PowerHelper* pPowerHelper,
            QWidget* pParent);

  private:
    void showProgress();
    void showResults();
    QWidget* rowWidget(int index);
    void askToInstallHelper(QWidget* pRow);

    const QPointer<system::SystemCheck> m_pCheck;
    const QPointer<CratePreferences> m_pPreferences;
    const QPointer<system::PowerHelper> m_pPowerHelper;
    QLabel* m_pProgress;
    QWidget* m_pRows;
    QVBoxLayout* m_pRowsLayout;
};

} // namespace crate
