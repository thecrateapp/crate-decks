#include "crate/ui/widgets/camelotwheelmodel.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QSignalSpy>
#include <QStringList>

#include "crate/ui/skin/crateskin.h"
#include "crate/ui/widgets/instrumentpainter.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"
#include "track/keyutils.h"

namespace {

using namespace crate::widgets;
using mixxx::track::io::key::ChromaticKey;
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

class CrateCamelotWheelModelTest : public MixxxTest {
  protected:
    void SetUp() override {
        const InstrumentStyle style = InstrumentStyle::fromVariables(skinVariables());
        ASSERT_TRUE(style.isValid());
        m_contextOpacity = style.opacityContext;
    }

    double m_contextOpacity = 0;
};

TEST_F(CrateCamelotWheelModelTest, MarksAdjacentAndRelativeKeysCompatible) {
    // The wheel speaks Camelot as KeyUtils' Lancelot notation reads it
    EXPECT_EQ(key(QStringLiteral("8A")), mixxx::track::io::key::A_MINOR);
    EXPECT_EQ(key(QStringLiteral("8B")), mixxx::track::io::key::C_MAJOR);

    CamelotWheelModel model(m_contextOpacity);
    ASSERT_EQ(model.nodes().size(), 24);
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
            QStringList compatible;
            for (const ChromaticKey compatibleKey : model.compatibleKeys()) {
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
