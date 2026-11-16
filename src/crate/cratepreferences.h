#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include "crate/analysis/analysisimporter.h"
#include "preferences/usersettings.h"

namespace crate {

/// What the user chose on the Crate page of the preferences for the audio
/// cache and Crate's analysis (plan F4.C-PR4), kept in the `[Crate]` group
/// of the settings. The components read it from here and follow its
/// signals:
/// - the cache directory: CrateServers creates the audio cache there, so a
///   new one takes effect at the next start (the files already cached stay
///   where they are);
/// - the cache limit: the audio cache evicts at once what it leaves out;
/// - Auto DJ prefetch: CratePrefetcher keeps the next tracks of the queue
///   downloaded, or not;
/// - keylock during Crate Smart Mix: the Auto DJ adapter's director turns it
///   on for the incoming deck, or leaves it;
/// - the Smart Mix assistant's transition: the length SmartMixAssist asks
///   the planner for, which asks again for the plan when it changes;
/// - the key from Crate and the gain from loudness: the AnalysisImporter
///   policy of the tracks taken from then on.
/// Without settings it keeps the defaults in memory.
class CratePreferences : public QObject {
    Q_OBJECT
  public:
    /// Design §9.
    static constexpr qint64 kDefaultCacheLimitBytes = qint64{20} << 30;
    /// The smallest limit the preferences take.
    static constexpr qint64 kMinimumCacheLimitBytes = qint64{1000} * 1000 * 1000;

    explicit CratePreferences(UserSettingsPointer pConfig, QObject* parent = nullptr);

    /// The directory chosen for the cache; empty for the default one.
    QString cacheDirectory() const;
    void setCacheDirectory(const QString& directory);

    qint64 cacheLimitBytes() const;
    /// At least kMinimumCacheLimitBytes.
    void setCacheLimitBytes(qint64 limitBytes);

    /// Whether the next tracks of Auto DJ are downloaded ahead. On by
    /// default.
    bool prefetchAutoDj() const;
    void setPrefetchAutoDj(bool enabled);

    /// Crate's key instead of the key detector's. Off by default (F0 Q5).
    bool keyFromCrate() const;
    void setKeyFromCrate(bool enabled);
    /// ReplayGain from Crate's loudness for tracks without one. Off by
    /// default.
    bool gainFromLufs() const;
    void setGainFromLufs(bool enabled);

    /// Keylock on the incoming deck of a Crate Smart Mix transition, so a
    /// beatmatch changes its tempo and not its key. On by default (Diego,
    /// 2026-11-15); off leaves each deck's own keylock as it is.
    bool smartMixKeylock() const;
    void setSmartMixKeylock(bool enabled);

    /// The transition length the Smart Mix assistant prefers, in seconds:
    /// its plans' `preferredDurationMs`. 8 s by default (the planner
    /// context's), from 1 s, the planner's shortest transition, to 12 s, the
    /// most the plan request takes. Longer blends, up to the plan's
    /// `policyLimits.maximumLongBeatmatchMs`, are the planner's choice, not
    /// this one.
    static constexpr int kDefaultAssistTransitionSeconds = 8;
    static constexpr int kMinimumAssistTransitionSeconds = 1;
    static constexpr int kMaximumAssistTransitionSeconds = 12;
    int smartMixAssistTransitionSeconds() const;
    /// Kept within the bounds above.
    void setSmartMixAssistTransitionSeconds(int seconds);

    /// Smart Mix Stems (STEM-D7): separate on the GPU when there is one
    /// that can, else on the CPU. On by default.
    bool stemsOnGpu() const;
    void setStemsOnGpu(bool enabled);
    /// The separation model: "htdemucs" by default, or "htdemucs_ft" (four
    /// fine-tuned models, about four times the work).
    QString stemModel() const;
    void setFineTunedStemModel(bool enabled);
    /// Separate while a deck plays, at the lowest priority. Off by default:
    /// separation pauses.
    bool separateWhilePlaying() const;
    void setSeparateWhilePlaying(bool enabled);

    /// Draw with the discrete GPU of a hybrid laptop (hybrid laptop plan §2).
    /// Off by default (Diego, 2026-11-15); main() applies it at the next
    /// start, before Qt creates any OpenGL context
    /// (system::applyDiscreteGpuPreference()).
    bool discreteGpu() const;
    void setDiscreteGpu(bool enabled);

    /// The import policy these choices make. Crate's beats stay off until
    /// the server announces the S8 analyzer.
    analysis::Policy analysisPolicy() const;

  signals:
    void cacheDirectoryChanged(const QString& directory);
    void cacheLimitChanged(qint64 limitBytes);
    void prefetchAutoDjChanged(bool enabled);
    void smartMixKeylockChanged(bool enabled);
    void smartMixAssistTransitionChanged(int seconds);
    void analysisPolicyChanged();
    void stemsChanged();
    void discreteGpuChanged(bool enabled);

  private:
    bool flag(const char* key, bool defaultValue) const;
    /// Stores the flag; returns whether it changed.
    bool setFlag(const char* key, bool value, bool defaultValue);

    const UserSettingsPointer m_pConfig;
    // Without settings
    QString m_cacheDirectory;
    qint64 m_cacheLimitBytes = kDefaultCacheLimitBytes;
    int m_assistTransitionSeconds = kDefaultAssistTransitionSeconds;
    QHash<QString, bool> m_flags;
};

} // namespace crate
