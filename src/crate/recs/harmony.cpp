#include "crate/recs/harmony.h"

#include <QCoreApplication>
#include <QStringView>
#include <optional>

namespace crate::recs {

namespace {

constexpr int kKeysPerRing = 12;
// Design 14.3: the energy boost is +7 on the same ring
constexpr int kEnergySteps = 7;

struct Camelot {
    int number = 0;
    QChar ring;
};

std::optional<Camelot> parse(const QString& text) {
    const QString key = text.trimmed().toUpper();
    if (key.size() < 2) {
        return std::nullopt;
    }
    const QChar ring = key.back();
    if (ring != QLatin1Char('A') && ring != QLatin1Char('B')) {
        return std::nullopt;
    }
    bool ok = false;
    const int number = QStringView(key).chopped(1).toInt(&ok);
    if (!ok || number < 1 || number > kKeysPerRing) {
        return std::nullopt;
    }
    return Camelot{number, ring};
}

/// `number` moved `steps` round the ring, 1 to 12.
int wrap(int number, int steps) {
    return ((number - 1 + steps) % kKeysPerRing + kKeysPerRing) % kKeysPerRing + 1;
}

QString tr(const char* text, const char* comment) {
    return QCoreApplication::translate("crate::recs::Harmony", text, comment);
}

} // namespace

Harmony harmonyOf(const QString& seedCamelot, const QString& camelot) {
    const std::optional<Camelot> seed = parse(seedCamelot);
    const std::optional<Camelot> key = parse(camelot);
    if (!seed || !key) {
        return Harmony::Unknown;
    }
    if (seed->ring != key->ring) {
        return seed->number == key->number ? Harmony::Relative : Harmony::Other;
    }
    if (key->number == seed->number) {
        return Harmony::Same;
    }
    if (key->number == wrap(seed->number, 1)) {
        return Harmony::Up;
    }
    if (key->number == wrap(seed->number, -1)) {
        return Harmony::Down;
    }
    if (key->number == wrap(seed->number, kEnergySteps)) {
        return Harmony::EnergyBoost;
    }
    return Harmony::Other;
}

bool isHarmonic(Harmony harmony) {
    return harmony != Harmony::Unknown && harmony != Harmony::Other;
}

QString harmonyLabel(Harmony harmony) {
    switch (harmony) {
    case Harmony::Same:
        return tr("Same key", "Harmony of a compatible track: the same key");
    case Harmony::Up:
        return tr("+1", "Harmony of a compatible track: one step up the Camelot wheel");
    case Harmony::Down:
        return tr("−1", "Harmony of a compatible track: one step down the Camelot wheel");
    case Harmony::Relative:
        return tr("Relative", "Harmony of a compatible track: the relative major or minor");
    case Harmony::EnergyBoost:
        return tr("Energy +7",
                "Harmony of a compatible track: seven steps up the wheel, an energy boost");
    case Harmony::Other:
        return QStringLiteral("—");
    case Harmony::Unknown:
        break;
    }
    return QString();
}

Harmony harmonyOfRelationship(const QString& harmonicRelationship) {
    if (harmonicRelationship == QLatin1String("same")) {
        return Harmony::Same;
    }
    if (harmonicRelationship == QLatin1String("adjacent")) {
        return Harmony::Up;
    }
    if (harmonicRelationship == QLatin1String("relative")) {
        return Harmony::Relative;
    }
    if (harmonicRelationship == QLatin1String("incompatible")) {
        return Harmony::Other;
    }
    return Harmony::Unknown;
}

QString camelotStep(const QString& camelot, int steps, bool relative) {
    const std::optional<Camelot> key = parse(camelot);
    if (!key) {
        return QString();
    }
    const QChar ring = relative
            ? (key->ring == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A'))
            : key->ring;
    return QString::number(wrap(key->number, steps)) + ring;
}

} // namespace crate::recs
