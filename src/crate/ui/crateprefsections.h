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
/// (level ring, use, size limit, location, prewarm and progressive play),
/// with the Auto DJ prefetch under the location (F4.C-PR4). Prewarm is plan
/// F5 and progressive playback F4b: their rows stay hidden until then.
class CacheSection : public PrefsSection {
    Q_OBJECT
  public:
    explicit CacheSection(QWidget* pParent);

    /// What the cache holds: `usedBytes` of `limitBytes`, `tracks` tracks,
    /// `downloading` of them on their way.
    void setUsage(qint64 usedBytes, qint64 limitBytes, int tracks, int downloading);
    /// The directory of the cache, and whether it takes effect only at the
    /// next start.
    void setLocation(const QString& path, bool atNextStart = false);
    /// The size limit's slider starts at `bytes`.
    void setMinimumLimit(qint64 bytes);

    QSlider* limit() const {
        return m_pLimit;
    }
    QLineEdit* location() const {
        return m_pLocation;
    }
    QLabel* locationHint() const {
        return m_pLocationHint;
    }
    QPushButton* chooseLocation() const {
        return m_pChoose;
    }
    QPushButton* clear() const {
        return m_pClear;
    }
    QCheckBox* prefetchAutoDj() const {
        return m_pPrefetch;
    }
    QCheckBox* prewarm() const {
        return m_pPrewarm;
    }
    QCheckBox* progressivePlayback() const {
        return m_pProgressive;
    }

  signals:
    /// The user let go of the size limit at `limitBytes`.
    void limitChosen(qint64 limitBytes);

  private:
    LevelGauge* m_pGauge;
    QLabel* m_pUsage;
    QLabel* m_pOf;
    QLabel* m_pCaption;
    QSlider* m_pLimit;
    QLabel* m_pLimitValue;
    QLineEdit* m_pLocation;
    QLabel* m_pLocationHint;
    QPushButton* m_pChoose;
    QPushButton* m_pClear;
    QCheckBox* m_pPrefetch;
    QCheckBox* m_pPrewarm;
    QCheckBox* m_pProgressive;
};

/// What of Crate's analysis replaces the local one (plan F4.C-PR2 and
/// F4.C-PR4): the key, and the gain from Crate's loudness. Crate's beat grid
/// waits for the S8 analyzer and has no switch yet.
class AnalysisSection : public PrefsSection {
    Q_OBJECT
  public:
    explicit AnalysisSection(QWidget* pParent);

    QCheckBox* keyFromCrate() const {
        return m_pKey;
    }
    QCheckBox* gainFromLufs() const {
        return m_pGain;
    }

  private:
    QCheckBox* m_pKey;
    QCheckBox* m_pGain;
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
