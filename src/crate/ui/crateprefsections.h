#pragma once

#include <QFrame>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QVBoxLayout;

namespace crate {

class LevelGauge;

/// A section of the Crate page (decks.css .psec): a 1 px rule, a MonoLabel
/// in the accent tone and its rows, without a box.
class PrefsSection : public QFrame {
    Q_OBJECT
  public:
    PrefsSection(const QString& title, QWidget* pParent);

    QVBoxLayout* rows() const {
        return m_pRows;
    }
    /// A row with a title, a hint in mono under it and a switch on the right
    /// (decks.css .trow); returns the switch.
    QCheckBox* addToggle(const QString& title, const QString& hint);

  private:
    QVBoxLayout* m_pRows;
};

/// The cache section of the Crate page, laid out as the GU1 mockups show it
/// (level ring, use, size limit, location, prewarm and progressive play).
/// The cache is plan F4 (F4.C-PR4 brings its preferences, F4b progressive
/// playback): until then the page keeps it hidden, and F4 connects it.
class CacheSection : public PrefsSection {
    Q_OBJECT
  public:
    explicit CacheSection(QWidget* pParent);

    /// What the cache holds: `usedBytes` of `limitBytes`, `tracks` tracks,
    /// `downloading` of them on their way.
    void setUsage(qint64 usedBytes, qint64 limitBytes, int tracks, int downloading);
    void setLocation(const QString& path);

    QCheckBox* prewarm() const {
        return m_pPrewarm;
    }
    QCheckBox* progressivePlayback() const {
        return m_pProgressive;
    }

  private:
    LevelGauge* m_pGauge;
    QLabel* m_pUsage;
    QLabel* m_pOf;
    QLabel* m_pCaption;
    QSlider* m_pLimit;
    QLabel* m_pLimitValue;
    QLineEdit* m_pLocation;
    QCheckBox* m_pPrewarm;
    QCheckBox* m_pProgressive;
};

/// The Auto DJ and Smart Mix section of the Crate page, as the GU1 mockups
/// show it: the Auto DJ transition (Crate Smart Mix, full intro and outro,
/// fade at the outro) and the Smart Mix switches. Smart Mix is plan F6: the
/// page keeps it hidden until F6 connects it.
class SmartMixSection : public PrefsSection {
    Q_OBJECT
  public:
    explicit SmartMixSection(QWidget* pParent);

    QPushButton* smartMix() const {
        return m_pSmartMix;
    }

  private:
    QPushButton* m_pSmartMix;
};

/// "18.4" and "of 40 GB": the cache's use in gigabytes, one decimal, and
/// its limit, as the mockups write them.
QString cacheUsageNumber(qint64 usedBytes);
QString cacheLimitText(qint64 limitBytes);

} // namespace crate
