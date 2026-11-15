#include "crate/smartmix/controlautomation.h"

#include <cmath>

#include "moc_controlautomation.cpp"

namespace crate {

ControlAutomation::ControlAutomation(const ConfigKey& key, QObject* pParent)
        : QObject(pParent),
          m_proxy(key, nullptr, ControlFlag::NoAssertIfMissing) {
}

void ControlAutomation::set(double value) {
    m_pending = value;
}

bool ControlAutomation::commit() {
    if (!m_pending) {
        return false;
    }
    const double target = *m_pending;
    m_pending.reset();
    // Never write over a change made since the last write.
    if (checkOverride() || !isValid()) {
        return false;
    }
    const double current = m_proxy.get();
    if (std::abs(current - target) <= kTolerance) {
        // Already there; from now on this value is the director's.
        m_lastWritten = current;
        return false;
    }
    m_proxy.set(target);
    // What the control kept, should it round or clamp the value.
    m_lastWritten = m_proxy.get();
    ++m_writeCount;
    return true;
}

bool ControlAutomation::write(double value) {
    set(value);
    return commit();
}

bool ControlAutomation::checkOverride() {
    if (m_overridden || !m_lastWritten || !isValid()) {
        return m_overridden;
    }
    if (std::abs(m_proxy.get() - *m_lastWritten) > kTolerance) {
        m_overridden = true;
        m_pending.reset();
        emit overridden(key());
    }
    return m_overridden;
}

void ControlAutomation::reset() {
    m_pending.reset();
    m_lastWritten.reset();
    m_overridden = false;
}

} // namespace crate
