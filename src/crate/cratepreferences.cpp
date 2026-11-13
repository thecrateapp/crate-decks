#include "crate/cratepreferences.h"

#include <QDir>
#include <algorithm>

#include "moc_cratepreferences.cpp"

namespace crate {

namespace {

const QString kGroup = QStringLiteral("[Crate]");
constexpr const char* kCacheDirectory = "CacheDirectory";
constexpr const char* kCacheLimitBytes = "CacheLimitBytes";
constexpr const char* kPrefetchAutoDj = "PrefetchAutoDj";
constexpr const char* kKeyFromCrate = "KeyFromCrate";
constexpr const char* kStemsOnGpu = "StemsOnGpu";
constexpr const char* kStemsFineTuned = "StemsFineTuned";
constexpr const char* kSeparateWhilePlaying = "SeparateWhilePlaying";
constexpr const char* kGainFromLufs = "GainFromLufs";

ConfigKey keyOf(const char* item) {
    return ConfigKey(kGroup, QLatin1String(item));
}

} // namespace

CratePreferences::CratePreferences(UserSettingsPointer pConfig, QObject* parent)
        : QObject(parent),
          m_pConfig(std::move(pConfig)) {
}

QString CratePreferences::cacheDirectory() const {
    return m_pConfig ? m_pConfig->getValueString(keyOf(kCacheDirectory)) : m_cacheDirectory;
}

void CratePreferences::setCacheDirectory(const QString& directory) {
    const QString cleaned = directory.trimmed().isEmpty() ? QString() : QDir::cleanPath(directory);
    if (cleaned == cacheDirectory()) {
        return;
    }
    if (m_pConfig) {
        m_pConfig->setValue(keyOf(kCacheDirectory), cleaned);
    } else {
        m_cacheDirectory = cleaned;
    }
    emit cacheDirectoryChanged(cleaned);
}

qint64 CratePreferences::cacheLimitBytes() const {
    if (!m_pConfig) {
        return m_cacheLimitBytes;
    }
    bool ok = false;
    const qint64 limitBytes = m_pConfig->getValueString(keyOf(kCacheLimitBytes)).toLongLong(&ok);
    return ok ? std::max(limitBytes, kMinimumCacheLimitBytes) : kDefaultCacheLimitBytes;
}

void CratePreferences::setCacheLimitBytes(qint64 limitBytes) {
    const qint64 limit = std::max(limitBytes, kMinimumCacheLimitBytes);
    if (limit == cacheLimitBytes()) {
        return;
    }
    if (m_pConfig) {
        m_pConfig->setValue(keyOf(kCacheLimitBytes), QString::number(limit));
    } else {
        m_cacheLimitBytes = limit;
    }
    emit cacheLimitChanged(limit);
}

bool CratePreferences::flag(const char* key, bool defaultValue) const {
    if (!m_pConfig) {
        return m_flags.value(QLatin1String(key), defaultValue);
    }
    return m_pConfig->getValue<bool>(keyOf(key), defaultValue);
}

bool CratePreferences::setFlag(const char* key, bool value, bool defaultValue) {
    if (flag(key, defaultValue) == value) {
        return false;
    }
    if (m_pConfig) {
        m_pConfig->setValue<bool>(keyOf(key), value);
    } else {
        m_flags.insert(QLatin1String(key), value);
    }
    return true;
}

bool CratePreferences::prefetchAutoDj() const {
    return flag(kPrefetchAutoDj, true);
}

void CratePreferences::setPrefetchAutoDj(bool enabled) {
    if (setFlag(kPrefetchAutoDj, enabled, true)) {
        emit prefetchAutoDjChanged(enabled);
    }
}

bool CratePreferences::stemsOnGpu() const {
    return flag(kStemsOnGpu, true);
}

void CratePreferences::setStemsOnGpu(bool enabled) {
    if (setFlag(kStemsOnGpu, enabled, true)) {
        emit stemsChanged();
    }
}

QString CratePreferences::stemModel() const {
    return flag(kStemsFineTuned, false) ? QStringLiteral("htdemucs_ft")
                                        : QStringLiteral("htdemucs");
}

void CratePreferences::setFineTunedStemModel(bool enabled) {
    if (setFlag(kStemsFineTuned, enabled, false)) {
        emit stemsChanged();
    }
}

bool CratePreferences::separateWhilePlaying() const {
    return flag(kSeparateWhilePlaying, false);
}

void CratePreferences::setSeparateWhilePlaying(bool enabled) {
    if (setFlag(kSeparateWhilePlaying, enabled, false)) {
        emit stemsChanged();
    }
}

bool CratePreferences::keyFromCrate() const {
    return flag(kKeyFromCrate, false);
}

void CratePreferences::setKeyFromCrate(bool enabled) {
    if (setFlag(kKeyFromCrate, enabled, false)) {
        emit analysisPolicyChanged();
    }
}

bool CratePreferences::gainFromLufs() const {
    return flag(kGainFromLufs, false);
}

void CratePreferences::setGainFromLufs(bool enabled) {
    if (setFlag(kGainFromLufs, enabled, false)) {
        emit analysisPolicyChanged();
    }
}

analysis::Policy CratePreferences::analysisPolicy() const {
    analysis::Policy policy;
    policy.crateKey = keyFromCrate();
    policy.gainFromLufs = gainFromLufs();
    return policy;
}

} // namespace crate
