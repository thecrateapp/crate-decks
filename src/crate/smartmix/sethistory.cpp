#include "crate/smartmix/sethistory.h"

#include <QPointer>
#include <algorithm>
#include <cmath>

#include "moc_sethistory.cpp"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::SetHistory");

/// The server's bounds of an entry (`SetHistoryEntryRequest`).
constexpr int kMaxDurationBars = 128;
/// Beats per bar without a time signature in the profile.
constexpr int kDefaultBeatsPerBar = 4;
/// Profiles kept for record(): two transitions' worth.
constexpr int kKeptProfiles = 8;

double roundedEnergy(double value) {
    return std::round(std::clamp(value, -1.0, 1.0) * 10000.0) / 10000.0;
}

} // namespace

QJsonObject SetHistoryEntry::toJson() const {
    return QJsonObject{
            {QStringLiteral("mode"), toString(mode)},
            {QStringLiteral("durationBars"), durationBars},
            {QStringLiteral("deltaEnergy"), deltaEnergy},
    };
}

SetHistory::SetHistory(Options options, QObject* pParent)
        : QObject(pParent),
          m_options(std::move(options)) {
}

void SetHistory::prepare(
        const QString& serverId, const QString& outgoingUid, const QString& incomingUid) {
    if (!m_options.profilesOf) {
        return;
    }
    m_options.profilesOf(serverId,
            {outgoingUid.toLower(), incomingUid.toLower()},
            [guard = QPointer(this)](const QList<analysis::MixProfile>& profiles) {
                if (!guard) {
                    return;
                }
                for (const analysis::MixProfile& profile : profiles) {
                    guard->keep(profile);
                }
            });
}

void SetHistory::keep(const analysis::MixProfile& profile) {
    const QString uid = profile.entityUid.toLower();
    m_profiles.insert(uid, profile);
    m_profileOrder.removeAll(uid);
    m_profileOrder.append(uid);
    while (m_profileOrder.size() > kKeptProfiles) {
        m_profiles.remove(m_profileOrder.takeFirst());
    }
}

void SetHistory::record(const TransitionPlan& plan, double durationMs) {
    const auto outgoing = m_profiles.constFind(plan.outgoingTrackEntityUid);
    const auto incoming = m_profiles.constFind(plan.incomingTrackEntityUid);
    if (outgoing == m_profiles.cend() || incoming == m_profiles.cend()) {
        kLogger.info() << "Not in the set history: no profiles of"
                       << plan.outgoingTrackEntityUid << "->" << plan.incomingTrackEntityUid;
        return;
    }
    record(plan, durationMs, outgoing.value(), incoming.value());
}

void SetHistory::record(const TransitionPlan& plan,
        double durationMs,
        const analysis::MixProfile& outgoing,
        const analysis::MixProfile& incoming) {
    const std::optional<SetHistoryEntry> entry = entryOf(plan, durationMs, outgoing, incoming);
    if (!entry) {
        kLogger.info() << "Not in the set history: the profiles give no tempo or energy";
        return;
    }
    m_entries.append(*entry);
    while (m_entries.size() > kSize) {
        m_entries.removeFirst();
    }
}

std::optional<SetHistoryEntry> SetHistory::entryOf(const TransitionPlan& plan,
        double durationMs,
        const analysis::MixProfile& outgoing,
        const analysis::MixProfile& incoming) {
    if (!outgoing.bpm || !(*outgoing.bpm > 0.0) || !outgoing.outroEnergy ||
            !incoming.introEnergy) {
        return std::nullopt;
    }
    const int beatsPerBar = outgoing.timeSignature && *outgoing.timeSignature > 0
            ? *outgoing.timeSignature
            : kDefaultBeatsPerBar;
    const double bars = std::max(0.0, durationMs) * *outgoing.bpm / 60000.0 / beatsPerBar;
    SetHistoryEntry entry;
    entry.mode = plan.mode;
    entry.durationBars =
            static_cast<int>(std::clamp<long>(std::lround(bars), 0, kMaxDurationBars));
    entry.deltaEnergy = roundedEnergy(*incoming.introEnergy - *outgoing.outroEnergy);
    return entry;
}

} // namespace crate
