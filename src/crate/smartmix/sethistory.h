#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "crate/analysis/mixprofile.h"
#include "crate/smartmix/transitionplan.h"

namespace crate {

/// One transition of the set, as the planner takes it in `context.setHistory`
/// (`SetHistoryEntryRequest`, SM3-D6): numbers only.
struct SetHistoryEntry {
    TransitionPlan::Mode mode = TransitionPlan::Mode::Adaptive;
    /// The transition's length in bars of the outgoing track, from 0 to 128.
    int durationBars = 0;
    /// The incoming intro's energy less the outgoing outro's, from -1 to 1.
    double deltaEnergy = 0.0;

    QJsonObject toJson() const;
    bool operator==(const SetHistoryEntry& other) const = default;
};

/// Options of crate::SetHistory.
struct SetHistoryOptions {
    using ProfilesDone = std::function<void(const QList<analysis::MixProfile>& profiles)>;
    /// Asks a server for the mix profiles of some tracks, and calls `done`
    /// once, now or later, with those it has. Without it, prepare() finds
    /// none.
    std::function<void(const QString& serverId,
            const QStringList& entityUids,
            const ProfilesDone& done)>
            profilesOf;
};

/// The last transitions the Smart Mix directors played in this session, for
/// `context.setHistory` of smart-mix-v3 (plan F6b.PR1): the mode, the length
/// in bars at the outgoing track's tempo and the step in energy from the
/// outgoing outro to the incoming intro, both from the tracks' mix profiles.
///
/// The caller of a director calls prepare() when it hands it a transition, so
/// the profiles are there when the director has played it, and record() then.
/// A transition whose profiles do not give those numbers (no tempo or no
/// energies) is not recorded. Lives on the main thread.
///
/// The planners of the server do not read the history yet (it was the AI
/// refinement's, dropped after SG3), so it changes no plan.
class SetHistory : public QObject {
    Q_OBJECT
  public:
    using Options = SetHistoryOptions;

    /// The most entries the server takes.
    static constexpr int kSize = 3;

    explicit SetHistory(Options options = Options(), QObject* pParent = nullptr);

    /// A transition from `outgoingUid` to `incomingUid` of `serverId` may be
    /// played: asks for their profiles now.
    void prepare(const QString& serverId, const QString& outgoingUid, const QString& incomingUid);
    /// A director played `plan` for `durationMs` of outgoing track time:
    /// records it with the profiles prepare() got.
    void record(const TransitionPlan& plan, double durationMs);
    /// The same with the profiles of its tracks.
    void record(const TransitionPlan& plan,
            double durationMs,
            const analysis::MixProfile& outgoing,
            const analysis::MixProfile& incoming);

    /// The entry of `plan` played for `durationMs` between tracks of these
    /// profiles, if they give its numbers.
    static std::optional<SetHistoryEntry> entryOf(const TransitionPlan& plan,
            double durationMs,
            const analysis::MixProfile& outgoing,
            const analysis::MixProfile& incoming);

    /// The last kSize entries, oldest first.
    const QList<SetHistoryEntry>& entries() const {
        return m_entries;
    }

  private:
    void keep(const analysis::MixProfile& profile);

    Options m_options;
    QList<SetHistoryEntry> m_entries;
    /// The profiles prepare() got, by lowercase uid; the last few only.
    QHash<QString, analysis::MixProfile> m_profiles;
    QStringList m_profileOrder;
};

} // namespace crate
