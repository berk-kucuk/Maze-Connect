#pragma once

#include <QtGlobal>

#include <bitset>

#include "mazeconnect/core/Limits.h"

namespace mazeconnect::core {

/**
 * Sliding-window replay detector over per-session monotonic message
 * counters.
 *
 * TLS 1.3 already prevents replay *within* a session. This sits above it to
 * stop a counter being reused after any resynchronisation, and to make the
 * ordering guarantee explicit and testable rather than implicit in the TLS
 * layer's behaviour.
 *
 * Counters start at 1; 0 is never valid, so a zero-initialised or truncated
 * field is rejected rather than accepted as "the first message".
 */
class ReplayWindow {
public:
    static constexpr std::size_t kWindowSize = limits::kReplayWindowSize;

    /**
     * Record @p counter as seen.
     *
     * Returns false — and changes nothing — if the counter is zero, has
     * already been seen, or has fallen so far behind the highest counter
     * that it can no longer be distinguished from a replay. The caller must
     * treat false as a protocol violation and drop the connection.
     */
    bool accept(quint64 counter);

    /// Highest counter accepted so far (0 before the first message).
    quint64 highest() const { return m_highest; }

    void reset();

private:
    quint64 m_highest = 0;
    std::bitset<kWindowSize> m_seen;
};

} // namespace mazeconnect::core
