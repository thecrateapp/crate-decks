#include "crate/writeback/cuemapping.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

#include "track/cue.h"
#include "track/track.h"
#include "util/color/predefinedcolorpalettes.h"
#include "util/color/rgbcolor.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::CueMapping");

const QString kTypeKey = QStringLiteral("cue_type");
const QString kSlotKey = QStringLiteral("slot");
const QString kStartKey = QStringLiteral("start_ms");
const QString kEndKey = QStringLiteral("end_ms");
const QString kLabelKey = QStringLiteral("label");
const QString kColorKey = QStringLiteral("color");

bool isSlotted(const QString& type) {
    return type == cuetype::kHotcue || type == cuetype::kLoop || type == cuetype::kJump;
}

/// Main, intro and outro first, then the slots in order.
int orderOf(const SyncedCue& cue) {
    if (cue.type == cuetype::kMain) {
        return 0;
    }
    if (cue.type == cuetype::kIntro) {
        return 1;
    }
    if (cue.type == cuetype::kOutro) {
        return 2;
    }
    return 3 + cue.slot;
}

/// The server rounds times to 0.001 ms.
double roundedMs(double ms) {
    return std::round(ms * 1000.0) / 1000.0;
}

std::optional<double> roundedMs(const std::optional<double>& ms) {
    return ms ? std::optional<double>(roundedMs(*ms)) : std::nullopt;
}

bool inRange(const std::optional<double>& ms) {
    return !ms || (std::isfinite(*ms) && *ms >= 0.0 && *ms <= kSyncedCueMaxMs);
}

/// The server's rules for each type (S7, "Cue rules").
bool followsRules(const SyncedCue& cue) {
    if (!inRange(cue.startMs) || !inRange(cue.endMs)) {
        return false;
    }
    if (isSlotted(cue.type)) {
        if (cue.slot < 0 || cue.slot >= kSyncedHotcueSlots) {
            return false;
        }
    } else if (cue.slot != 0) {
        return false;
    }
    if (cue.type == cuetype::kHotcue || cue.type == cuetype::kMain) {
        return cue.startMs && !cue.endMs;
    }
    if (cue.type == cuetype::kLoop) {
        return cue.startMs && cue.endMs && *cue.endMs > *cue.startMs;
    }
    if (cue.type == cuetype::kJump) {
        return cue.startMs && cue.endMs && *cue.endMs != *cue.startMs;
    }
    if (cue.type == cuetype::kIntro || cue.type == cuetype::kOutro) {
        if (!cue.startMs && !cue.endMs) {
            return false;
        }
        return !cue.startMs || !cue.endMs || *cue.endMs > *cue.startMs;
    }
    return false;
}

QString canonicalLabel(const QString& label) {
    return label.trimmed().isEmpty() ? QString() : label.left(kSyncedLabelMaxLength);
}

/// Sorts, and keeps the first cue of each type and slot and of each hot cue
/// slot.
SyncedCues canonical(SyncedCues cues) {
    std::stable_sort(cues.begin(), cues.end(), [](const SyncedCue& a, const SyncedCue& b) {
        return orderOf(a) < orderOf(b);
    });
    SyncedCues unique;
    QSet<int> orders;
    for (const SyncedCue& cue : std::as_const(cues)) {
        const int order = orderOf(cue);
        if (orders.contains(order)) {
            continue;
        }
        orders.insert(order);
        unique.append(cue);
    }
    return unique;
}

std::optional<double> optionalMs(const QJsonValue& value) {
    return value.isDouble() ? std::optional<double>(value.toDouble()) : std::nullopt;
}

QJsonValue jsonOf(const std::optional<double>& ms) {
    return ms ? QJsonValue(*ms) : QJsonValue(QJsonValue::Null);
}

QJsonValue jsonOf(const QString& text) {
    return text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(text);
}

std::optional<mixxx::CueType> upstreamTypeOf(const QString& type) {
    if (type == cuetype::kMain) {
        return mixxx::CueType::MainCue;
    }
    if (type == cuetype::kIntro) {
        return mixxx::CueType::Intro;
    }
    if (type == cuetype::kOutro) {
        return mixxx::CueType::Outro;
    }
    if (type == cuetype::kHotcue) {
        return mixxx::CueType::HotCue;
    }
    if (type == cuetype::kLoop) {
        return mixxx::CueType::Loop;
    }
    if (type == cuetype::kJump) {
        return mixxx::CueType::Jump;
    }
    return std::nullopt;
}

QString typeOf(mixxx::CueType type) {
    switch (type) {
    case mixxx::CueType::MainCue:
        return cuetype::kMain;
    case mixxx::CueType::Intro:
        return cuetype::kIntro;
    case mixxx::CueType::Outro:
        return cuetype::kOutro;
    case mixxx::CueType::HotCue:
        return cuetype::kHotcue;
    case mixxx::CueType::Loop:
        return cuetype::kLoop;
    case mixxx::CueType::Jump:
        return cuetype::kJump;
    case mixxx::CueType::Invalid:
    case mixxx::CueType::Beat:
    case mixxx::CueType::N60dBSound:
        break;
    }
    return QString();
}

mixxx::audio::FramePos framesOf(
        const std::optional<double>& ms, mixxx::audio::SampleRate sampleRate) {
    if (!ms) {
        return mixxx::audio::kInvalidFramePos;
    }
    return mixxx::audio::FramePos(*ms * sampleRate.toDouble() / 1000.0);
}

} // namespace

SyncedCues syncedCuesOf(const TrackPointer& pTrack) {
    SyncedCues cues;
    if (!pTrack) {
        return cues;
    }
    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    if (!sampleRate.isValid()) {
        return cues;
    }
    const QList<CuePointer> cuePoints = pTrack->getCuePoints();
    for (const CuePointer& pCue : cuePoints) {
        const QString type = typeOf(pCue->getType());
        if (type.isEmpty()) {
            continue;
        }
        const mixxx::CueInfo info = pCue->getCueInfo(sampleRate);
        SyncedCue cue;
        cue.type = type;
        cue.startMs = roundedMs(info.getStartPositionMillis());
        cue.endMs = roundedMs(info.getEndPositionMillis());
        if (isSlotted(type)) {
            // A loop or jump without a hot cue is not saved: the temporary loop
            cue.slot = info.getHotCueIndex().value_or(-1);
            cue.label = canonicalLabel(info.getLabel());
            if (info.getColor()) {
                cue.color = mixxx::RgbColor::toQString(*info.getColor()).toLower();
            }
        } else if (type == cuetype::kMain) {
            // Upstream keeps no end for the main cue
            cue.endMs.reset();
        }
        if (followsRules(cue)) {
            cues.append(cue);
        }
    }
    return canonical(cues);
}

bool applySyncedCues(const TrackPointer& pTrack, const SyncedCues& cues) {
    if (!pTrack) {
        return false;
    }
    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    if (!sampleRate.isValid()) {
        return false;
    }
    // What the contract has no place for stays
    QList<CuePointer> cuePoints;
    const QList<CuePointer> current = pTrack->getCuePoints();
    for (const CuePointer& pCue : current) {
        const mixxx::CueType type = pCue->getType();
        const int hotCue = pCue->getHotCue();
        const bool slotted = (type == mixxx::CueType::HotCue || type == mixxx::CueType::Loop ||
                                     type == mixxx::CueType::Jump) &&
                hotCue >= 0 && hotCue < kSyncedHotcueSlots;
        const bool single = type == mixxx::CueType::MainCue || type == mixxx::CueType::Intro ||
                type == mixxx::CueType::Outro;
        if (!slotted && !single) {
            cuePoints.append(pCue);
        }
    }
    for (const SyncedCue& cue : cues) {
        const std::optional<mixxx::CueType> type = upstreamTypeOf(cue.type);
        if (!type || !followsRules(cue)) {
            continue;
        }
        const mixxx::RgbColor color = mixxx::RgbColor::fromQString(cue.color).value_or(
                mixxx::PredefinedColorPalettes::kDefaultCueColor);
        CuePointer pCue(new Cue(*type,
                isSlotted(cue.type) ? cue.slot : Cue::kNoHotCue,
                framesOf(cue.startMs, sampleRate),
                framesOf(cue.endMs, sampleRate),
                color));
        if (!cue.label.isEmpty()) {
            pCue->setLabel(cue.label);
        }
        cuePoints.append(pCue);
    }
    pTrack->setCuePoints(cuePoints);
    return true;
}

SyncedCues syncedCuesFromJson(const QJsonArray& json) {
    static const QRegularExpression kColor(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    SyncedCues cues;
    for (const QJsonValue& value : json) {
        const QJsonObject object = value.toObject();
        SyncedCue cue;
        cue.type = object.value(kTypeKey).toString();
        cue.slot = object.value(kSlotKey).toInt(0);
        cue.startMs = roundedMs(optionalMs(object.value(kStartKey)));
        cue.endMs = roundedMs(optionalMs(object.value(kEndKey)));
        cue.label = canonicalLabel(object.value(kLabelKey).toString());
        const QString color = object.value(kColorKey).toString();
        if (kColor.match(color).hasMatch()) {
            cue.color = color.toLower();
        }
        if (!upstreamTypeOf(cue.type) || !followsRules(cue)) {
            kLogger.warning() << "Skipping a synced cue that is not one:" << object;
            continue;
        }
        if (!isSlotted(cue.type)) {
            cue.label.clear();
            cue.color.clear();
        }
        cues.append(cue);
    }
    return canonical(cues);
}

QJsonArray toJson(const SyncedCues& cues) {
    QJsonArray json;
    for (const SyncedCue& cue : cues) {
        json.append(QJsonObject{
                {kTypeKey, cue.type},
                {kSlotKey, cue.slot},
                {kStartKey, jsonOf(cue.startMs)},
                {kEndKey, jsonOf(cue.endMs)},
                {kLabelKey, jsonOf(cue.label)},
                {kColorKey, jsonOf(cue.color)},
        });
    }
    return json;
}

CueMerge mergeLocalCues(const SyncedCues& server, const SyncedCues& local) {
    CueMerge merge;
    merge.cues = server;
    QSet<int> taken;
    for (const SyncedCue& cue : server) {
        if (isSlotted(cue.type)) {
            taken.insert(cue.slot);
        }
    }
    const auto alreadyThere = [&server](const SyncedCue& cue) {
        return std::any_of(server.cbegin(), server.cend(), [&cue](const SyncedCue& other) {
            return isSlotted(other.type) && other.type == cue.type &&
                    other.startMs == cue.startMs && other.endMs == cue.endMs;
        });
    };
    SyncedCues homeless;
    for (const SyncedCue& cue : local) {
        if (!isSlotted(cue.type) || alreadyThere(cue)) {
            continue;
        }
        if (taken.contains(cue.slot)) {
            homeless.append(cue);
            continue;
        }
        taken.insert(cue.slot);
        merge.cues.append(cue);
    }
    for (SyncedCue cue : std::as_const(homeless)) {
        int slot = 0;
        while (slot < kSyncedHotcueSlots && taken.contains(slot)) {
            ++slot;
        }
        if (slot >= kSyncedHotcueSlots) {
            merge.dropped.append(cue);
            continue;
        }
        cue.slot = slot;
        taken.insert(slot);
        merge.cues.append(cue);
        ++merge.moved;
    }
    merge.cues = canonical(merge.cues);
    return merge;
}

SyncedCues automaticCues(const SyncedCues& cues) {
    SyncedCues automatic;
    for (const SyncedCue& cue : cues) {
        if (!isSlotted(cue.type)) {
            automatic.append(cue);
        }
    }
    return automatic;
}

SyncedCues withAutomaticCues(const SyncedCues& cues, const SyncedCues& automatic) {
    SyncedCues merged = automaticCues(automatic);
    for (const SyncedCue& cue : cues) {
        if (isSlotted(cue.type)) {
            merged.append(cue);
        }
    }
    return canonical(merged);
}

} // namespace crate::writeback
