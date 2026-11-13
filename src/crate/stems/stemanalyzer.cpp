#include "crate/stems/stemanalyzer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>

namespace crate {

namespace {

constexpr double kLevelFloorDb = -60.0;
constexpr double kBytesPerDb = 255.0 / -kLevelFloorDb;
/// Upper bounds of the server for untrusted documents.
constexpr qint64 kMaxDurationMs = 86400000;
constexpr qsizetype kMaxDrumBeats = 100000;

bool fail(QString* pError, const QString& message) {
    if (pError) {
        *pError = message;
    }
    return false;
}

/// RMS in dBFS of a mean square; -inf for silence.
double levelDb(double meanSquare) {
    return meanSquare > 0.0 ? 10.0 * std::log10(meanSquare)
                            : -std::numeric_limits<double>::infinity();
}

bool matches(const QString& pattern, const QString& value) {
    return QRegularExpression(QRegularExpression::anchoredPattern(pattern)).match(value).hasMatch();
}

} // namespace

const QString StemAnalysis::kSchema = QStringLiteral("stem-analysis/1");
const std::array<QString, 4> StemAnalysis::kSources = {
        QStringLiteral("drums"),
        QStringLiteral("bass"),
        QStringLiteral("other"),
        QStringLiteral("vocals"),
};
const QString StemAnalysis::kSeparatorVersion = QStringLiteral("htdemucs-onnx-1");

int StemAnalysis::separatorStrength() const {
    if (separatorModel == QLatin1String("htdemucs_ft")) {
        return 2;
    }
    return separatorModel == QLatin1String("htdemucs") ? 1 : 0;
}

QByteArray StemAnalysis::toJson() const {
    QJsonObject levelsObject;
    for (std::size_t stem = 0; stem < kSources.size(); ++stem) {
        levelsObject.insert(kSources[stem], QString::fromLatin1(levels[stem].toBase64()));
    }
    QJsonArray sources;
    for (const QString& source : kSources) {
        sources.append(source);
    }
    QJsonObject document{
            {QStringLiteral("schema"), kSchema},
            {QStringLiteral("producer"), producer},
            {QStringLiteral("separator"),
                    QJsonObject{
                            {QStringLiteral("model"), separatorModel},
                            {QStringLiteral("version"), separatorVersion},
                            {QStringLiteral("sources"), sources},
                    }},
            {QStringLiteral("sourceRevision"), sourceRevision},
            {QStringLiteral("hopMs"), hopMs},
            {QStringLiteral("levels"), levelsObject},
            {QStringLiteral("durationMs"), durationMs},
    };
    if (!drumBeatsMs.isEmpty()) {
        QJsonArray beats;
        for (const qint64 beat : drumBeatsMs) {
            beats.append(beat);
        }
        document.insert(QStringLiteral("drumBeatsMs"), beats);
    }
    return QJsonDocument(document).toJson(QJsonDocument::Compact);
}

std::optional<StemAnalysis> StemAnalysis::fromJson(const QByteArray& json, QString* pError) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (!document.isObject()) {
        fail(pError, QStringLiteral("Not a JSON object: %1").arg(parseError.errorString()));
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    StemAnalysis analysis;
    if (object.value(QStringLiteral("schema")).toString() != kSchema) {
        fail(pError, QStringLiteral("Not a %1 document").arg(kSchema));
        return std::nullopt;
    }
    analysis.producer = object.value(QStringLiteral("producer")).toString();
    if (!matches(QStringLiteral("crate-(server|decks)/[0-9A-Za-z][0-9A-Za-z.+-]{0,63}"),
                analysis.producer)) {
        fail(pError, QStringLiteral("Unknown producer"));
        return std::nullopt;
    }
    const QJsonObject separator = object.value(QStringLiteral("separator")).toObject();
    analysis.separatorModel = separator.value(QStringLiteral("model")).toString();
    analysis.separatorVersion = separator.value(QStringLiteral("version")).toString();
    if (analysis.separatorStrength() == 0 ||
            !matches(QStringLiteral("[0-9A-Za-z][0-9A-Za-z._+-]{0,63}"),
                    analysis.separatorVersion)) {
        fail(pError, QStringLiteral("Unknown separator"));
        return std::nullopt;
    }
    const QJsonArray sources = separator.value(QStringLiteral("sources")).toArray();
    bool sourcesMatch = sources.size() == static_cast<qsizetype>(kSources.size());
    for (qsizetype i = 0; sourcesMatch && i < sources.size(); ++i) {
        sourcesMatch = sources.at(i).toString() == kSources[static_cast<std::size_t>(i)];
    }
    if (!sourcesMatch) {
        fail(pError, QStringLiteral("The sources are not drums, bass, other and vocals"));
        return std::nullopt;
    }
    analysis.sourceRevision = object.value(QStringLiteral("sourceRevision")).toString();
    if (!matches(QStringLiteral("[0-9a-f]{64}"), analysis.sourceRevision)) {
        fail(pError, QStringLiteral("sourceRevision is not 64 hex digits"));
        return std::nullopt;
    }
    if (object.value(QStringLiteral("hopMs")).toInteger(-1) != kHopMs) {
        fail(pError, QStringLiteral("hopMs is not %1").arg(kHopMs));
        return std::nullopt;
    }
    analysis.durationMs = object.value(QStringLiteral("durationMs")).toInteger(-1);
    if (analysis.durationMs <= 0 || analysis.durationMs > kMaxDurationMs) {
        fail(pError, QStringLiteral("durationMs is out of range"));
        return std::nullopt;
    }
    const QJsonObject levels = object.value(QStringLiteral("levels")).toObject();
    for (std::size_t stem = 0; stem < kSources.size(); ++stem) {
        const QJsonValue value = levels.value(kSources[stem]);
        const auto decoded = QByteArray::fromBase64Encoding(value.toString().toLatin1(),
                QByteArray::AbortOnBase64DecodingErrors);
        if (!value.isString() || !decoded) {
            fail(pError, QStringLiteral("The %1 levels are not base64").arg(kSources[stem]));
            return std::nullopt;
        }
        analysis.levels[stem] = *decoded;
    }
    const qsizetype windows = analysis.levels[0].size();
    for (const QByteArray& stemLevels : analysis.levels) {
        if (stemLevels.size() != windows) {
            fail(pError, QStringLiteral("Every stem needs the same number of windows"));
            return std::nullopt;
        }
    }
    const qint64 coveredMs = windows * kHopMs;
    if (coveredMs > analysis.durationMs + kHopMs) {
        fail(pError, QStringLiteral("The levels are longer than the duration plus a window"));
        return std::nullopt;
    }
    if (coveredMs == 0 || coveredMs < analysis.durationMs - kHopMs) {
        fail(pError, QStringLiteral("The levels are shorter than the duration minus a window"));
        return std::nullopt;
    }
    const QJsonValue beats = object.value(QStringLiteral("drumBeatsMs"));
    if (!beats.isUndefined() && !beats.isNull()) {
        const QJsonArray beatArray = beats.toArray();
        if (!beats.isArray() || beatArray.size() > kMaxDrumBeats) {
            fail(pError, QStringLiteral("drumBeatsMs is not a list of beats"));
            return std::nullopt;
        }
        qint64 previous = -1;
        for (const QJsonValue& beat : beatArray) {
            const qint64 beatMs = beat.toInteger(-1);
            if (!beat.isDouble() || beatMs <= previous || beatMs > analysis.durationMs) {
                fail(pError,
                        QStringLiteral("drumBeatsMs must increase within the analysed audio"));
                return std::nullopt;
            }
            analysis.drumBeatsMs.append(beatMs);
            previous = beatMs;
        }
    }
    return analysis;
}

QString decksStemProducer() {
    QString version = QCoreApplication::applicationVersion();
    static const QRegularExpression kOther(QStringLiteral("[^0-9A-Za-z.+-]"));
    version.replace(kOther, QStringLiteral("-"));
    if (version.isEmpty() || !version.at(0).isLetterOrNumber()) {
        version.prepend(QLatin1Char('0'));
    }
    return QStringLiteral("crate-decks/") + version.left(64);
}

quint8 encodeStemLevel(double rmsDb) {
    if (std::isnan(rmsDb)) {
        return 0;
    }
    const double clipped = std::clamp(rmsDb, kLevelFloorDb, 0.0);
    return static_cast<quint8>(std::floor((clipped - kLevelFloorDb) * kBytesPerDb + 0.5));
}

StemAnalyzer::StemAnalyzer(int sampleRate)
        : m_sampleRate(sampleRate),
          m_window(static_cast<SINT>(sampleRate) * StemAnalysis::kHopMs / 1000) {
}

void StemAnalyzer::reset() {
    m_frames = 0;
    m_levels = {};
    m_squares = {};
    m_inWindow = 0;
}

bool StemAnalyzer::begin(const StemSeparationSource& source, QString* pError) {
    Q_UNUSED(source);
    Q_UNUSED(pError);
    reset();
    return true;
}

bool StemAnalyzer::write(
        SINT firstFrame, SINT frameCount, std::span<const float> stems, QString* pError) {
    if (firstFrame != m_frames) {
        return fail(pError, QStringLiteral("The stems are not in order"));
    }
    if (stems.size() < static_cast<std::size_t>(8 * frameCount)) {
        return fail(pError, QStringLiteral("Fewer stems than frames"));
    }
    SINT frame = 0;
    while (frame < frameCount) {
        const SINT count = std::min(frameCount - frame, m_window - m_inWindow);
        for (std::size_t stem = 0; stem < 4; ++stem) {
            const float* pLeft = stems.data() + 2 * stem * frameCount + frame;
            const float* pRight = pLeft + frameCount;
            double squares = m_squares[stem];
            for (SINT i = 0; i < count; ++i) {
                // The mean of both channels, as numpy's in float64
                const double mono =
                        (static_cast<double>(pLeft[i]) + static_cast<double>(pRight[i])) / 2.0;
                squares += mono * mono;
            }
            m_squares[stem] = squares;
        }
        m_inWindow += count;
        frame += count;
        if (m_inWindow == m_window) {
            for (std::size_t stem = 0; stem < 4; ++stem) {
                m_levels[stem].append(static_cast<char>(encodeStemLevel(
                        levelDb(m_squares[stem] / static_cast<double>(m_window)))));
            }
            m_squares = {};
            m_inWindow = 0;
        }
    }
    m_frames += frameCount;
    return true;
}

StemAnalysis StemAnalyzer::finish(const QString& sourceRevision, const QString& producer) const {
    StemAnalysis analysis;
    analysis.producer = producer;
    analysis.sourceRevision = sourceRevision;
    analysis.levels = m_levels;
    if (m_inWindow > 0) {
        // The last window covers the samples it has
        for (std::size_t stem = 0; stem < 4; ++stem) {
            analysis.levels[stem].append(static_cast<char>(encodeStemLevel(
                    levelDb(m_squares[stem] / static_cast<double>(m_inWindow)))));
        }
    }
    analysis.durationMs = std::llround(static_cast<double>(m_frames) * 1000.0 / m_sampleRate);
    return analysis;
}

} // namespace crate
