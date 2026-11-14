#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include "proto/keys.pb.h"

namespace crate::widgets {

/// What the Camelot wheel of design 14.3 shows, without widgets: which key
/// is the master deck's, which keys are compatible with it (±1, the relative
/// key and +7 energy), which are in the current results, and which are
/// unrelated context at opacityContext. CamelotWheel paints it; stage 2
/// can draw it in QML without changing it.
///
/// The keys in the results are a plain setter: the Compatibles view of F5,
/// which will feed it with the keys of its list, does not exist yet.
class CamelotWheelModel : public QObject {
    Q_OBJECT
  public:
    using ChromaticKey = mixxx::track::io::key::ChromaticKey;

    /// How a key relates to the master deck's (the legend of the GU1
    /// mockup library-compatibles).
    enum class Relation {
        /// No master track, or a key in the results: a hollow node.
        Rest,
        /// ±1 on the same ring, the relative key or +7: accentHi stroke and
        /// an active edge with an arrow.
        Compatible,
        /// The master deck's key: filled in accent.
        Master,
        /// No relation and no results: at opacityContext.
        Context,
    };

    struct Node {
        ChromaticKey key = mixxx::track::io::key::INVALID;
        /// Camelot number, 1 to 12
        int number = 0;
        /// B, the outer ring; A (minor), the inner one
        bool major = false;
        /// "8A"
        QString camelot;
        /// The key's traditional name as KeyUtils writes it ("Am")
        QString name;
        Relation relation = Relation::Rest;
        double opacity = 1;
    };

    /// `contextOpacity` is the opacity of unrelated keys (the style's
    /// opacityContext).
    explicit CamelotWheelModel(double contextOpacity, QObject* pParent = nullptr);

    /// The master deck's key; INVALID when there is no master track.
    void setMasterKey(ChromaticKey key);
    ChromaticKey masterKey() const {
        return m_masterKey;
    }

    /// The keys of the tracks in the current results.
    void setKeysInResults(const QList<ChromaticKey>& keys);

    /// The 24 keys of the wheel: 1A to 12A, then 1B to 12B.
    const QVector<Node>& nodes() const {
        return m_nodes;
    }
    /// The index of `key` in nodes(), or -1.
    int indexOf(ChromaticKey key) const;
    Relation relationOf(ChromaticKey key) const;
    double opacityOf(ChromaticKey key) const;

    /// The keys compatible with the master deck's, in the order of their
    /// edges: -1, +1, the relative key and +7. Empty without a master key.
    QVector<ChromaticKey> compatibleKeys() const;

    /// `key` in Camelot ("8A"), or an empty string.
    static QString camelotOf(ChromaticKey key);

  signals:
    /// Something the wheel shows has changed.
    void changed();

  private:
    void update();

    const double m_contextOpacity;
    ChromaticKey m_masterKey = mixxx::track::io::key::INVALID;
    /// Whether each node is in the results
    QVector<bool> m_inResults;
    QVector<Node> m_nodes;
};

} // namespace crate::widgets
