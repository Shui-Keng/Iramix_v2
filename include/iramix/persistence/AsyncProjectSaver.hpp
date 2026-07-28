#pragma once

#include "iramix/persistence/ProjectStore.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace iramix::persistence {

using ImmutableProjectPayload =
    std::shared_ptr<const std::vector<std::byte>>;

enum class ProjectSaveSubmitResult {
    accepted,
    full,
    invalidRevision,
    stopped,
};

enum class ProjectSaveCompletionStatus {
    committed,
    failed,
};

struct ProjectSaveCompletion final {
    std::uint64_t revision {0U};
    ProjectSaveCompletionStatus status {
        ProjectSaveCompletionStatus::failed
    };
    std::array<char, 192> detail {};
};

// A bounded single-producer/control-thread -> single-worker pipeline.
// A slot remains occupied until its durable completion is consumed, so save
// requests and ACK/REJECT records cannot be silently dropped independently.
class AsyncProjectSaver final {
public:
    ~AsyncProjectSaver();

    AsyncProjectSaver(const AsyncProjectSaver&) = delete;
    AsyncProjectSaver& operator=(const AsyncProjectSaver&) = delete;

    [[nodiscard]] static std::unique_ptr<AsyncProjectSaver> create(
        std::filesystem::path target,
        std::uint32_t pipelineCapacity,
        std::string& error
    );

    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;

    [[nodiscard]] ProjectSaveSubmitResult trySubmit(
        std::uint64_t revision,
        const ImmutableProjectPayload& payload,
        AtomicSaveFailurePoint failurePoint =
            AtomicSaveFailurePoint::none
    ) noexcept;

    [[nodiscard]] bool tryPopCompletion(
        ProjectSaveCompletion& completion
    ) noexcept;

    [[nodiscard]] std::uint64_t outstandingCount() const noexcept;
    [[nodiscard]] std::uint64_t pendingSaveCount() const noexcept;
    [[nodiscard]] std::uint64_t completionCount() const noexcept;
    [[nodiscard]] std::uint64_t acceptedCount() const noexcept;
    [[nodiscard]] std::uint64_t rejectedCount() const noexcept;

private:
    struct Impl;
    explicit AsyncProjectSaver(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace iramix::persistence
