#pragma once

#include <QObject>
#include <optional>

#include "control/controlproxy.h"
#include "preferences/configobject.h"

namespace crate {

/// One control the Smart Mix director moves (a knob, a fader, a deck's
/// tempo), through a ControlProxy that remembers what the director wrote.
///
/// The director never fights the DJ: when the control holds another value
/// than the last one written here, something else moved it (the DJ, a
/// controller, the engine), and the control becomes *overridden*. It then
/// stays where that left it, nothing is written to it again until reset(),
/// and overridden() is emitted once. Only this control stops being
/// automated; the director goes on with the others.
///
/// Writes go in two steps, so a control is written at most once per tick of
/// the director, whatever the tick computes: set() keeps the latest target
/// and commit() writes it, only if it differs from the control's value.
class ControlAutomation : public QObject {
    Q_OBJECT
  public:
    /// Values closer than this to the last write count as the same one, so a
    /// control that rounds what it is given is not taken for an override.
    static constexpr double kTolerance = 1e-6;

    explicit ControlAutomation(const ConfigKey& key, QObject* pParent = nullptr);

    const ConfigKey& key() const {
        return m_proxy.getKey();
    }
    /// The control exists. A missing one (no EQ on the deck, for instance)
    /// reads 0 and is never written.
    bool isValid() const {
        return m_proxy.valid();
    }
    bool isOverridden() const {
        return m_overridden;
    }
    double get() const {
        return m_proxy.get();
    }

    /// Takes `value` as the target of the next commit(); a later set() in
    /// the same tick replaces it.
    void set(double value);
    /// Writes the target of the last set(), if any, unless the control is
    /// overridden (checkOverride() first) or already holds it. Returns
    /// whether it wrote.
    bool commit();
    /// set() and commit() at once.
    bool write(double value);

    /// Marks the control overridden, and emits overridden(), if its value is
    /// no longer the one this automation wrote last. Returns isOverridden().
    bool checkOverride();

    /// Forgets the last write, the pending target and any override: the
    /// control is automated again from the next write.
    void reset();

    /// How many times this automation has written the control.
    int writeCount() const {
        return m_writeCount;
    }

  signals:
    void overridden(const ConfigKey& key);

  private:
    ControlProxy m_proxy;
    std::optional<double> m_pending;
    std::optional<double> m_lastWritten;
    bool m_overridden = false;
    int m_writeCount = 0;
};

} // namespace crate
