#include "crate/ui/widgets/camelotwheelmodel.h"

#include "library/library_prefs.h"
#include "moc_camelotwheelmodel.cpp"

namespace crate::widgets {

namespace {

constexpr int kKeysPerRing = 12;
// Design 14.3: the energy boost is +7 on the same ring
constexpr int kEnergyStep = 7;

int wrap(int number) {
    return (number - 1 + kKeysPerRing) % kKeysPerRing + 1;
}

int indexOfPosition(int number, bool major) {
    return (major ? kKeysPerRing : 0) + number - 1;
}

} // namespace

CamelotWheelModel::CamelotWheelModel(double contextOpacity, QObject* pParent)
        : QObject(pParent),
          m_contextOpacity(contextOpacity),
          // Missing only outside the app (tests, tools): the wheel then
          // reads KeyUtils' own notation
          m_keyNotation(mixxx::library::prefs::kKeyNotationConfigKey,
                  this,
                  ControlFlag::NoWarnIfMissing),
          m_inResults(2 * kKeysPerRing, false) {
    m_nodes.reserve(2 * kKeysPerRing);
    // The wheel's keys come from KeyUtils' own reading of Camelot (Lancelot)
    // notation: no table of keys of our own
    for (const bool major : {false, true}) {
        for (int number = 1; number <= kKeysPerRing; ++number) {
            Node node;
            node.number = number;
            node.major = major;
            node.camelot = QString::number(number) + QLatin1Char(major ? 'B' : 'A');
            node.key = KeyUtils::guessKeyFromText(node.camelot);
            m_nodes.append(node);
        }
    }
    setNotation(KeyUtils::keyNotationFromNumericValue(m_keyNotation.get()));
    // The preferences set the control on OK even when the notation stays
    // Custom and only its names change, so every set renames the keys
    m_keyNotation.connectValueChanged(this, [this](double value) {
        setNotation(KeyUtils::keyNotationFromNumericValue(value));
    });
}

void CamelotWheelModel::setNotation(KeyUtils::KeyNotation notation) {
    m_notation = notation == KeyUtils::KeyNotation::Invalid ? KeyUtils::KeyNotation::Custom
                                                            : notation;
    bool renamed = false;
    for (Node& node : m_nodes) {
        QString label = labelOf(node.key);
        if (label != node.label) {
            node.label = std::move(label);
            renamed = true;
        }
    }
    if (renamed) {
        emit changed();
    }
}

QString CamelotWheelModel::labelOf(ChromaticKey key) const {
    return KeyUtils::keyToString(key, m_notation);
}

QString CamelotWheelModel::camelotOf(ChromaticKey key) {
    return KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot);
}

int CamelotWheelModel::indexOf(ChromaticKey key) const {
    if (key == mixxx::track::io::key::INVALID) {
        return -1;
    }
    for (int index = 0; index < m_nodes.size(); ++index) {
        if (m_nodes.at(index).key == key) {
            return index;
        }
    }
    return -1;
}

void CamelotWheelModel::setMasterKey(ChromaticKey key) {
    if (indexOf(key) < 0) {
        key = mixxx::track::io::key::INVALID;
    }
    if (key == m_masterKey) {
        return;
    }
    m_masterKey = key;
    update();
    emit changed();
}

void CamelotWheelModel::setKeysInResults(const QList<ChromaticKey>& keys) {
    QVector<bool> inResults(2 * kKeysPerRing, false);
    for (const ChromaticKey key : keys) {
        const int index = indexOf(key);
        if (index >= 0) {
            inResults[index] = true;
        }
    }
    if (inResults == m_inResults) {
        return;
    }
    m_inResults = inResults;
    update();
    emit changed();
}

QVector<CamelotWheelModel::ChromaticKey> CamelotWheelModel::compatibleKeys() const {
    const int master = indexOf(m_masterKey);
    if (master < 0) {
        return {};
    }
    const Node& node = m_nodes.at(master);
    return {
            m_nodes.at(indexOfPosition(wrap(node.number - 1), node.major)).key,
            m_nodes.at(indexOfPosition(wrap(node.number + 1), node.major)).key,
            m_nodes.at(indexOfPosition(node.number, !node.major)).key,
            m_nodes.at(indexOfPosition(wrap(node.number + kEnergyStep), node.major)).key,
    };
}

CamelotWheelModel::Relation CamelotWheelModel::relationOf(ChromaticKey key) const {
    const int index = indexOf(key);
    return index < 0 ? Relation::Context : m_nodes.at(index).relation;
}

double CamelotWheelModel::opacityOf(ChromaticKey key) const {
    const int index = indexOf(key);
    return index < 0 ? m_contextOpacity : m_nodes.at(index).opacity;
}

void CamelotWheelModel::update() {
    const QVector<ChromaticKey> compatible = compatibleKeys();
    for (int index = 0; index < m_nodes.size(); ++index) {
        Node& node = m_nodes[index];
        if (m_masterKey == mixxx::track::io::key::INVALID) {
            node.relation = Relation::Rest;
        } else if (node.key == m_masterKey) {
            node.relation = Relation::Master;
        } else if (compatible.contains(node.key)) {
            node.relation = Relation::Compatible;
        } else if (m_inResults.at(index)) {
            node.relation = Relation::Rest;
        } else {
            node.relation = Relation::Context;
        }
        node.opacity = node.relation == Relation::Context ? m_contextOpacity : 1;
    }
}

} // namespace crate::widgets
