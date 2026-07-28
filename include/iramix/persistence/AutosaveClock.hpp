#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace iramix::persistence {

// Time source for the autosave deadline. Injected so a test can drive the
// fixed window exactly instead of racing it: with the real clock the
// window is only observable by sleeping longer than it, which makes any
// assertion about coalescing depend on filesystem latency.
class AutosaveClock {
public:
    using Observer = std::function<void()>;

    AutosaveClock() = default;
    virtual ~AutosaveClock() = default;

    AutosaveClock(const AutosaveClock&) = delete;
    AutosaveClock& operator=(const AutosaveClock&) = delete;

    [[nodiscard]] virtual std::chrono::steady_clock::time_point now()
        const noexcept = 0;

    // The real time at which a waiter should re-check the deadline.
    // std::nullopt means this clock does not advance on its own, so a
    // waiter must block until notified rather than until a wall-clock
    // instant that would never be meaningful.
    [[nodiscard]] virtual std::optional<
        std::chrono::steady_clock::time_point>
    wakeTimeFor(
        std::chrono::steady_clock::time_point deadline
    ) const noexcept = 0;

    // Invoked after the clock moves. A clock that cannot move on its own
    // uses this to wake whoever is waiting on it.
    virtual void setObserver(Observer observer) = 0;
};

// Production clock: virtual time is real time.
class SteadyAutosaveClock final : public AutosaveClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now()
        const noexcept override;

    [[nodiscard]] std::optional<
        std::chrono::steady_clock::time_point>
    wakeTimeFor(
        std::chrono::steady_clock::time_point deadline
    ) const noexcept override;

    void setObserver(Observer observer) override;
};

// Test clock: advances only when advance() is called, which then wakes the
// waiter. Deadlines become exact rather than approximate.
class ManualAutosaveClock final : public AutosaveClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now()
        const noexcept override;

    [[nodiscard]] std::optional<
        std::chrono::steady_clock::time_point>
    wakeTimeFor(
        std::chrono::steady_clock::time_point deadline
    ) const noexcept override;

    void setObserver(Observer observer) override;

    void advance(std::chrono::milliseconds delta);

private:
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point now_ {
        std::chrono::steady_clock::time_point {}
        + std::chrono::hours {1}
    };
    Observer observer_;
};

[[nodiscard]] std::shared_ptr<AutosaveClock> makeSteadyAutosaveClock();

} // namespace iramix::persistence
