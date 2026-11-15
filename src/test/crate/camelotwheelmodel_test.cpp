#include "crate/ui/widgets/camelotwheelmodel.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QSignalSpy>
#include <QStringList>

#include "control/controlobject.h"
#include "crate/ui/skin/crateskin.h"
#include "crate/ui/widgets/instrumentpainter.h"
#include "library/library_prefs.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"
#include "track/keyutils.h"

namespace {

using namespace crate::widgets;
using mixxx::track::io::key::ChromaticKey;
using Notation = KeyUtils::KeyNotation;
using Relation = CamelotWheelModel::Relation;

/// The values of the CrateDecks skin, as the skin loads them
crate::SkinVariables skinVariables() {
    return crate::readSkinVariables(
            QDir(ConfigObject<ConfigValue>::computeResourcePath())
                    .filePath(QStringLiteral("skins/CrateDecks/generated/vars.xml")));
}

ChromaticKey key(const QString& camelot) {
    return KeyUtils::guessKeyFromText(camelot);
}

QString camelot(int number, QChar letter) {
    return QString::number(number) + letter;
}

int wrap(int number) {
    return (number - 1 + 12) % 12 + 1;
}

/// The label of the node at `camelot`
QString labelAt(const CamelotWheelModel& model, const QString& camelot) {
    return model.nodes().at(model.indexOf(key(camelot))).label;
}

class CrateCamelotWheelModelTest : public MixxxTest {
  protected:
    void SetUp() override {
        const InstrumentStyle style = InstrumentStyle::fromVariables(skinVariables());
        ASSERT_TRUE(style.isValid());
        m_contextOpacity = style.opacityContext;
    }

    void TearDown() override {
        // The custom notation is KeyUtils' global map
        KeyUtils::setNotation({});
    }

    double m_contextOpacity = 0;
};

TEST_F(CrateCamelotWheelModelTest, MarksAdjacentAndRelativeKeysCompatible) {
    // The wheel speaks Camelot as KeyUtils' Lancelot notation reads it
    EXPECT_EQ(key(QStringLiteral("8A")), mixxx::track::io::key::A_MINOR);
    EXPECT_EQ(key(QStringLiteral("8B")), mixxx::track::io::key::C_MAJOR);

    CamelotWheelModel model(m_contextOpacity);
    ASSERT_EQ(model.nodes().size(), 24);
    QStringList compatible;
    for (const QChar letter : {QLatin1Char('A'), QLatin1Char('B')}) {
        const QChar other = letter == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
        for (int number = 1; number <= 12; ++number) {
            const QString master = camelot(number, letter);
            model.setMasterKey(key(master));
            ASSERT_EQ(model.masterKey(), key(master)) << master.toStdString();

            // ±1 on the same ring, the relative key (A <-> B) and +7 energy
            const QStringList expected = {
                    camelot(wrap(number - 1), letter),
                    camelot(wrap(number + 1), letter),
                    camelot(number, other),
                    camelot(wrap(number + 7), letter),
            };
            compatible.clear();
            const auto compatibleKeys = model.compatibleKeys();
            for (const ChromaticKey compatibleKey : compatibleKeys) {
                compatible.append(CamelotWheelModel::camelotOf(compatibleKey));
            }
            EXPECT_EQ(compatible, expected) << master.toStdString();

            for (const CamelotWheelModel::Node& node : model.nodes()) {
                const Relation relation = node.camelot == master
                        ? Relation::Master
                        : expected.contains(node.camelot) ? Relation::Compatible
                                                          : Relation::Context;
                EXPECT_EQ(node.relation, relation)
                        << master.toStdString() << " " << node.camelot.toStdString();
                EXPECT_EQ(model.relationOf(node.key), relation);
            }
        }
    }

    // Not a mirror of the arithmetic: the keys of the GU1 mockup and the
    // wrap at both ends of the wheel
    model.setMasterKey(key(QStringLiteral("8A")));
    EXPECT_EQ(model.relationOf(key(QStringLiteral("3A"))), Relation::Compatible);
    EXPECT_EQ(model.relationOf(key(QStringLiteral("8B"))), Relation::Compatible);
    EXPECT_EQ(model.relationOf(key(QStringLiteral("9B"))), Relation::Context);
    model.setMasterKey(key(QStringLiteral("12B")));
    EXPECT_EQ(model.relationOf(key(QStringLiteral("1B"))), Relation::Compatible);
    EXPECT_EQ(model.relationOf(key(QStringLiteral("7B"))), Relation::Compatible);
    model.setMasterKey(key(QStringLiteral("1A")));
    EXPECT_EQ(model.relationOf(key(QStringLiteral("12A"))), Relation::Compatible);
    EXPECT_EQ(model.relationOf(key(QStringLiteral("8A"))), Relation::Compatible);
}

TEST_F(CrateCamelotWheelModelTest, NamesKeysInTheChosenNotation) {
    CamelotWheelModel model(m_contextOpacity);
    const QString a1 = QStringLiteral("1A");
    const QString a8 = QStringLiteral("8A");
    const QString b2 = QStringLiteral("2B");

    model.setNotation(Notation::Lancelot);
    EXPECT_EQ(labelAt(model, a1), QStringLiteral("1A"));
    EXPECT_EQ(labelAt(model, a8), QStringLiteral("8A"));
    EXPECT_EQ(labelAt(model, b2), QStringLiteral("2B"));

    model.setNotation(Notation::OpenKey);
    EXPECT_EQ(labelAt(model, a1), QStringLiteral("6m"));
    EXPECT_EQ(labelAt(model, a8), QStringLiteral("1m"));
    EXPECT_EQ(labelAt(model, b2), QStringLiteral("7d"));

    model.setNotation(Notation::Traditional);
    EXPECT_EQ(labelAt(model, a1), QStringLiteral("G♯m"));
    EXPECT_EQ(labelAt(model, a8), QStringLiteral("Am"));
    EXPECT_EQ(labelAt(model, b2), QStringLiteral("F♯/G♭"));

    // A combined notation labels the wheel with its code only; the full
    // name is for the selected key, in the centre
    model.setNotation(Notation::LancelotAndTraditional);
    EXPECT_EQ(labelAt(model, a8), QStringLiteral("8A"));
    EXPECT_EQ(labelAt(model, b2), QStringLiteral("2B"));
    EXPECT_EQ(model.labelOf(key(a8)), QStringLiteral("8A (Am)"));
    EXPECT_EQ(model.labelOf(key(b2)), QStringLiteral("2B (F♯/G♭)"));

    model.setNotation(Notation::OpenKeyAndTraditional);
    EXPECT_EQ(labelAt(model, a1), QStringLiteral("6m"));
    EXPECT_EQ(labelAt(model, a8), QStringLiteral("1m"));
    EXPECT_EQ(labelAt(model, b2), QStringLiteral("7d"));
    EXPECT_EQ(model.labelOf(key(a8)), QStringLiteral("1m (Am)"));
    EXPECT_EQ(model.labelOf(key(b2)), QStringLiteral("7d (F♯/G♭)"));

    // The custom notation is the one the user typed in the preferences
    KeyUtils::setNotation({{mixxx::track::io::key::A_MINOR, QStringLiteral("a-moll")}});
    model.setNotation(Notation::Custom);
    EXPECT_EQ(labelAt(model, a8), QStringLiteral("a-moll"));

    // The wheel stays a Camelot wheel whatever the notation: every label is
    // the key of its Camelot position, as KeyUtils writes it in the
    // notation (the code of a combined one)
    const QList<QPair<Notation, Notation>> notations = {
            {Notation::Lancelot, Notation::Lancelot},
            {Notation::OpenKey, Notation::OpenKey},
            {Notation::Traditional, Notation::Traditional},
            {Notation::LancelotAndTraditional, Notation::Lancelot},
            {Notation::OpenKeyAndTraditional, Notation::OpenKey},
    };
    for (const auto& [notation, code] : notations) {
        model.setNotation(notation);
        EXPECT_EQ(model.notation(), notation);
        for (const CamelotWheelModel::Node& node : model.nodes()) {
            EXPECT_EQ(node.key, key(node.camelot)) << node.camelot.toStdString();
            EXPECT_EQ(node.label, KeyUtils::keyToString(node.key, code))
                    << node.camelot.toStdString();
            EXPECT_EQ(model.labelOf(node.key), KeyUtils::keyToString(node.key, notation))
                    << node.camelot.toStdString();
        }
    }
    EXPECT_TRUE(model.labelOf(mixxx::track::io::key::INVALID).isEmpty());
}

TEST_F(CrateCamelotWheelModelTest, FollowsTheKeyNotationPreference) {
    // The control Preferences > Key detection sets, as Library creates it
    ControlObject keyNotation(mixxx::library::prefs::kKeyNotationConfigKey, false);
    keyNotation.set(static_cast<double>(Notation::Lancelot));
    CamelotWheelModel model(m_contextOpacity);
    EXPECT_EQ(model.notation(), Notation::Lancelot);
    EXPECT_EQ(labelAt(model, QStringLiteral("8A")), QStringLiteral("8A"));

    QSignalSpy changed(&model, &CamelotWheelModel::changed);
    keyNotation.set(static_cast<double>(Notation::Traditional));
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(model.notation(), Notation::Traditional);
    EXPECT_EQ(labelAt(model, QStringLiteral("8A")), QStringLiteral("Am"));

    // The same notation again changes no label: nothing to repaint
    keyNotation.set(static_cast<double>(Notation::Traditional));
    EXPECT_EQ(changed.count(), 1);

    // On OK, the preferences store the custom names and set the control,
    // even when it already was Custom
    KeyUtils::setNotation({{mixxx::track::io::key::A_MINOR, QStringLiteral("a-moll")}});
    keyNotation.set(static_cast<double>(Notation::Custom));
    EXPECT_EQ(changed.count(), 2);
    EXPECT_EQ(labelAt(model, QStringLiteral("8A")), QStringLiteral("a-moll"));
    KeyUtils::setNotation({{mixxx::track::io::key::A_MINOR, QStringLiteral("la menor")}});
    keyNotation.set(static_cast<double>(Notation::Custom));
    EXPECT_EQ(changed.count(), 3);
    EXPECT_EQ(labelAt(model, QStringLiteral("8A")), QStringLiteral("la menor"));
}

TEST_F(CrateCamelotWheelModelTest, NoMasterTrackMeansAllRest) {
    CamelotWheelModel model(m_contextOpacity);
    model.setKeysInResults({key(QStringLiteral("5A")), key(QStringLiteral("10A"))});
    EXPECT_EQ(model.masterKey(), mixxx::track::io::key::INVALID);
    EXPECT_TRUE(model.compatibleKeys().isEmpty());
    for (const CamelotWheelModel::Node& node : model.nodes()) {
        EXPECT_EQ(node.relation, Relation::Rest) << node.camelot.toStdString();
        EXPECT_DOUBLE_EQ(node.opacity, 1.0) << node.camelot.toStdString();
    }

    // A master deck that unloads its track leaves the wheel at rest again
    model.setMasterKey(key(QStringLiteral("8A")));
    model.setMasterKey(mixxx::track::io::key::INVALID);
    for (const CamelotWheelModel::Node& node : model.nodes()) {
        EXPECT_EQ(node.relation, Relation::Rest) << node.camelot.toStdString();
    }
}

TEST_F(CrateCamelotWheelModelTest, ContextOpacityForUnrelated) {
    EXPECT_DOUBLE_EQ(m_contextOpacity, 0.25);
    CamelotWheelModel model(m_contextOpacity);
    QSignalSpy changed(&model, &CamelotWheelModel::changed);
    // The wheel of the GU1 mockup (library-compatibles): 8A on the master
    // deck, 5A and 10A in the results besides the compatible keys
    model.setMasterKey(key(QStringLiteral("8A")));
    model.setKeysInResults({key(QStringLiteral("5A")),
            key(QStringLiteral("10A")),
            key(QStringLiteral("9A"))});
    EXPECT_EQ(changed.count(), 2);

    const QStringList full = {QStringLiteral("8A"),
            QStringLiteral("7A"),
            QStringLiteral("9A"),
            QStringLiteral("8B"),
            QStringLiteral("3A"),
            QStringLiteral("5A"),
            QStringLiteral("10A")};
    int context = 0;
    for (const CamelotWheelModel::Node& node : model.nodes()) {
        if (full.contains(node.camelot)) {
            EXPECT_DOUBLE_EQ(node.opacity, 1.0) << node.camelot.toStdString();
            EXPECT_NE(node.relation, Relation::Context) << node.camelot.toStdString();
        } else {
            EXPECT_EQ(node.relation, Relation::Context) << node.camelot.toStdString();
            EXPECT_DOUBLE_EQ(node.opacity, m_contextOpacity) << node.camelot.toStdString();
            ++context;
        }
        EXPECT_DOUBLE_EQ(model.opacityOf(node.key), node.opacity);
    }
    EXPECT_EQ(context, 24 - full.size());
    EXPECT_EQ(model.relationOf(key(QStringLiteral("5A"))), Relation::Rest);

    // The widget repaints only when the model changes
    model.setMasterKey(key(QStringLiteral("8A")));
    model.setKeysInResults({key(QStringLiteral("10A")),
            key(QStringLiteral("9A")),
            key(QStringLiteral("5A"))});
    EXPECT_EQ(changed.count(), 2);
}

} // namespace
