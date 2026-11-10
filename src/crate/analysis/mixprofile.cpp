#include "crate/analysis/mixprofile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

namespace crate::analysis {

namespace {

/// Reads the optional fields; the first one of the wrong type sets `error`.
class Reader {
  public:
    explicit Reader(const QJsonObject& object)
            : m_object(object) {
    }

    QString string(const char* key) {
        const QJsonValue value = m_object.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull()) {
            return QString();
        }
        if (!value.isString()) {
            fail(key);
            return QString();
        }
        return value.toString();
    }

    std::optional<double> number(const char* key) {
        const QJsonValue value = m_object.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull()) {
            return std::nullopt;
        }
        if (!value.isDouble()) {
            fail(key);
            return std::nullopt;
        }
        return value.toDouble();
    }

    std::optional<qint64> milliseconds(const char* key) {
        const std::optional<double> value = number(key);
        if (!value) {
            return std::nullopt;
        }
        if (*value < 0) {
            fail(key);
            return std::nullopt;
        }
        return qRound64(*value);
    }

    QVector<qint64> grid(const char* key) {
        const QJsonValue value = m_object.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull()) {
            return {};
        }
        if (!value.isArray()) {
            fail(key);
            return {};
        }
        const QJsonArray array = value.toArray();
        QVector<qint64> beats;
        beats.reserve(array.size());
        for (const QJsonValue& beat : array) {
            if (!beat.isDouble() || beat.toDouble() < 0 ||
                    (!beats.isEmpty() && qRound64(beat.toDouble()) <= beats.last())) {
                fail(key);
                return {};
            }
            beats.append(qRound64(beat.toDouble()));
        }
        return beats;
    }

    const QString& error() const {
        return m_error;
    }

    void fail(const char* key) {
        if (m_error.isEmpty()) {
            m_error = QStringLiteral("Invalid mix profile field %1").arg(QLatin1String(key));
        }
    }

  private:
    const QJsonObject& m_object;
    QString m_error;
};

std::optional<MixProfile::Quality> qualityOf(const QString& text) {
    if (text == QLatin1String("full")) {
        return MixProfile::Quality::Full;
    }
    if (text == QLatin1String("partial")) {
        return MixProfile::Quality::Partial;
    }
    if (text == QLatin1String("legacy")) {
        return MixProfile::Quality::Legacy;
    }
    if (text == QLatin1String("unavailable")) {
        return MixProfile::Quality::Unavailable;
    }
    return std::nullopt;
}

} // namespace

Result<MixProfile> parseMixProfile(const QJsonObject& object) {
    Reader reader(object);
    MixProfile profile;
    profile.entityUid = reader.string("trackEntityUid").toLower();
    profile.profileRevision = reader.string("profileRevision");
    profile.analyzer = reader.string("analyzer");
    profile.analyzerVersion = reader.string("analyzerVersion");
    const std::optional<MixProfile::Quality> quality = qualityOf(reader.string("quality"));
    if (!quality) {
        reader.fail("quality");
    } else {
        profile.quality = *quality;
    }
    profile.durationMs = reader.milliseconds("durationMs").value_or(0);
    profile.bpm = reader.number("bpm");
    profile.tempoStability = reader.number("tempoStability");
    profile.beatAnchorMs = reader.milliseconds("beatAnchorMs");
    profile.downbeatAnchorMs = reader.milliseconds("downbeatAnchorMs");
    if (const auto timeSignature = reader.number("timeSignature")) {
        profile.timeSignature = static_cast<int>(*timeSignature);
    }
    profile.beatGridMs = reader.grid("beatGridMs");
    profile.camelot = reader.string("camelot");
    profile.introCueMs = reader.milliseconds("introCueMs");
    profile.outroCueMs = reader.milliseconds("outroCueMs");
    profile.activeStartMs = reader.milliseconds("activeStartMs");
    profile.activeEndMs = reader.milliseconds("activeEndMs");
    profile.integratedLufs = reader.number("integratedLufs");
    profile.truePeakDbfs = reader.number("truePeakDbfs");
    if (profile.entityUid.isEmpty()) {
        reader.fail("trackEntityUid");
    }
    if (profile.profileRevision.isEmpty()) {
        reader.fail("profileRevision");
    }
    if (!reader.error().isEmpty()) {
        return Error::decode(reader.error());
    }
    return profile;
}

Result<MixProfile> parseMixProfile(const QByteArray& json) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return Error::decode(QStringLiteral("A mix profile is not a JSON object"));
    }
    return parseMixProfile(document.object());
}

} // namespace crate::analysis
