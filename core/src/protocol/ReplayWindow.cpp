#include "mazeconnect/core/ReplayWindow.h"

namespace mazeconnect::core {

void ReplayWindow::reset() {
    m_highest = 0;
    m_seen.reset();
}

bool ReplayWindow::accept(quint64 counter) {
    // Counters are 1-based so that an absent/zeroed field is never mistaken
    // for a legitimate first message.
    if (counter == 0) {
        return false;
    }

    if (counter > m_highest) {
        const quint64 shift = counter - m_highest;
        if (shift >= kWindowSize) {
            // Jumped clear past the window — nothing older is representable
            // any more, so start fresh from this counter.
            m_seen.reset();
        } else {
            m_seen <<= static_cast<std::size_t>(shift);
        }
        m_seen.set(0);
        m_highest = counter;
        return true;
    }

    const quint64 age = m_highest - counter;
    if (age >= kWindowSize) {
        // Too old to prove it isn't a replay; refuse rather than guess.
        return false;
    }

    const auto bit = static_cast<std::size_t>(age);
    if (m_seen.test(bit)) {
        return false; // already seen — replay
    }
    m_seen.set(bit);
    return true;
}

} // namespace mazeconnect::core
