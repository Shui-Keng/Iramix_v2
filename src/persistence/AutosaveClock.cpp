#include "iramix/persistence/AutosaveClock.hpp"

#include <utility>

namespace iramix::persistence {

std::chrono::steady_clock::time_point
SteadyAutosaveClock::now() const noexcept {
    return std::chrono::steady_clock::now();
}

std::optional<std::chrono::steady_clock::time_point>
SteadyAutosaveClock::wakeTimeFor(
    const std::chrono::steady_clock::time_point deadline
) const noexcept {
    return deadline;
}

void SteadyAutosaveClock::setObserver(Observer) {
    // Real time advances without help, so nothing needs waking.
}

std::chrono::steady_clock::time_point
ManualAutosaveClock::now() const noexcept {
    std::scoped_lock lock {mutex_};
    return now_;
}

std::optional<std::chrono::steady_clock::time_point>
ManualAutosaveClock::wakeTimeFor(
    std::chrono::steady_clock::time_point
) const noexcept {
    // Virtual time never reaches a deadline by itself: advance() is the
    // only thing that can, and it notifies.
    return std::nullopt;
}

void ManualAutosaveClock::setObserver(Observer observer) {
    std::scoped_lock lock {mutex_};
    observer_ = std::move(observer);
}

void ManualAutosaveClock::advance(
    const std::chrono::milliseconds delta
) {
    Observer observer;
    {
        std::scoped_lock lock {mutex_};
        now_ += delta;
        observer = observer_;
    }
    // Notify outside the lock so an observer that re-enters the clock
    // cannot deadlock against advance().
    if (observer) {
        observer();
    }
}

std::shared_ptr<AutosaveClock> makeSteadyAutosaveClock() {
    return std::make_shared<SteadyAutosaveClock>();
}

} // namespace iramix::persistence
