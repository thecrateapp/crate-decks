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

    /// The import policy these choices make. Crate's beats stay off until
    /// the server announces the S8 analyzer.
    analysis::Policy analysisPolicy() const;

  signals:
    void cacheDirectoryChanged(const QString& directory);
    void cacheLimitChanged(qint64 limitBytes);
    void prefetchAutoDjChanged(bool enabled);
    void analysisPolicyChanged();

  private:
    bool flag(const char* key, bool defaultValue) const;
    /// Stores the flag; returns whether it changed.
    bool setFlag(const char* key, bool value, bool defaultValue);

    const UserSettingsPointer m_pConfig;
    // Without settings
    QString m_cacheDirectory;
    qint64 m_cacheLimitBytes = kDefaultCacheLimitBytes;
    QHash<QString, bool> m_flags;
};

} // namespace crate
