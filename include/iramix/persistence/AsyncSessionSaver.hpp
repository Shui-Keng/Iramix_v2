#pragma once

#include "iramix/persistence/AsyncProjectSaver.hpp"
#include "iramix/persistence/SessionDocument.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace iramix::persistence {

using ImmutableSessionSnapshot =
    std::shared_ptr<const SessionDocument>;

struct SessionSaveCompletion final {
    std::uint64_t revision {0U};
    ProjectSaveCompletionStatus status {
        ProjectSaveCompletionStatus::failed
    };
    std::uint64_t serializedBytes {0U};
    std::uint64_t serializationNanoseconds {0U};
    std::uint64_t durableSaveNanoseconds {0U};
    std::array<char, 192> detail {};
};

// A bounded control-thread -> serialization/durable-save worker. Submission
// only transfers an immutable snapshot reference; validation, allocation,
// serialization, filesystem calls, and failure injection all happen off the
// caller thread.
class AsyncSessionSaver final {
public:
    ~AsyncSessionSaver();

    AsyncSessionSaver(const AsyncSessionSaver&) = delete;
    AsyncSessionSaver& operator=(const AsyncSessionSaver&) = delete;

    [[nodiscard]] static std::unique_ptr<AsyncSessionSaver> create(
        std::filesystem::path target,
        std::uint32_t pipelineCapacity,
        std::string& error
    );

    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;

    [[nodiscard]] ProjectSaveSubmitResult trySubmit(
        std::uint64_t revision,
        const ImmutableSessionSnapshot& snapshot,
        AtomicSaveFailurePoint failurePoint =
            AtomicSaveFailurePoint::none
    ) noexcept;

    [[nodiscard]] bool tryPopCompletion(
        SessionSaveCompletion& completion
    ) noexcept;

    [[nodiscard]] std::uint64_t outstandingCount() const noexcept;
    [[nodiscard]] std::uint64_t pendingSaveCount() const noexcept;
    [[nodiscard]] std::uint64_t completionCount() const noexcept;
    [[nodiscard]] std::uint64_t acceptedCount() const noexcept;
    [[nodiscard]] std::uint64_t rejectedCount() const noexcept;

private:
    struct Impl;
    explicit AsyncSessionSaver(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace iramix::persistence
