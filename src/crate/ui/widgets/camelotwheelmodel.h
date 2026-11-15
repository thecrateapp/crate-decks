#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include "control/controlproxy.h"
#include "proto/keys.pb.h"
#include "track/keyutils.h"

namespace crate::widgets {

/// What the Camelot wheel of design 14.3 shows, without widgets: which key
/// is the master deck's, which keys are compatible with it (±1, the relative
/// key and +7 energy), which are in the current results, and which are
/// unrelated context at opacityContext. CamelotWheel paints it; stage 2
/// can draw it in QML without changing it.
///
/// The keys in the results are a plain setter: the Compatibles view of F5,
/// which will feed it with the keys of its list, does not exist yet.
///
/// The wheel is always a Camelot wheel, but it names each key in the
/// notation of Preferences > Key detection, as the library's key column and
/// the decks do: it follows the [Library],key_notation control that the
/// preferences set, so it must be created after Library creates it. A
/// combined notation labels the keys of the wheel with their code ("8A")
/// and keeps the full name ("8A (Am)") for the selected key (labelOf()).
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
        /// The key in the chosen notation, as KeyUtils writes it ("8A",
        /// "1m", "Am"); only the code of a combined notation ("8A")
        QString label;
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

    /// Names the keys in `notation`. The model calls it when the key
    /// notation preference changes; Invalid means the notation the
    /// preferences gave KeyUtils (Custom).
    void setNotation(KeyUtils::KeyNotation notation);
    KeyUtils::KeyNotation notation() const {
        return m_notation;
    }

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
    /// `key` in the chosen notation, in full ("8A (Am)"), or an empty
    /// string.
    QString labelOf(ChromaticKey key) const;

  signals:
    /// Something the wheel shows has changed.
    void changed();

  private:
    void update();

    const double m_contextOpacity;
    ControlProxy m_keyNotation;
    KeyUtils::KeyNotation m_notation = KeyUtils::KeyNotation::Invalid;
    ChromaticKey m_masterKey = mixxx::track::io::key::INVALID;
    /// Whether each node is in the results
    QVector<bool> m_inResults;
    QVector<Node> m_nodes;
};

} // namespace crate::widgets
