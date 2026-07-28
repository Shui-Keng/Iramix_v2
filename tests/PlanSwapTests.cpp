#include "iramix/audio/Graph.hpp"
#include "iramix/audio/Node.hpp"
#include "iramix/audio/RenderPlan.hpp"
#include "iramix/audio/RenderPlanExecutor.hpp"
#include "iramix/realtime/Audit.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class ConstantNode final : public iramix::audio::IAudioNode {
public:
    explicit ConstantNode(const float value)
        : value_ {value} {}

    void prepare(
        const iramix::audio::NodePrepareInfo&
    ) override {}

    void process(
        const iramix::audio::NodeProcessContext& context
    ) noexcept override {
        if (context.audio.channelCount() == 0) {
            return;
        }
        float* const output = context.audio.channel(0);
        for (int frame = 0;
             frame < context.audio.frameCount();
             ++frame) {
            output[frame] = value_;
        }
    }

private:
    float value_ {0.0F};
};

class OutputNode final : public iramix::audio::IAudioNode {
public:
    void prepare(
        const iramix::audio::NodePrepareInfo&
    ) override {}

    void process(
        const iramix::audio::NodeProcessContext&
    ) noexcept override {}
};

} // namespace

int main() {
    constexpr int replacementCount = 1'000;
    constexpr int blockSize = 64;

    iramix::audio::GraphDescription graph;
    require(graph.addNode(1U), "source node");
    require(graph.addNode(2U), "output node");
    require(
        graph.addConnection({1U, 0, 2U, 0}),
        "source-to-output edge"
    );
    const iramix::audio::NodeInfoMap info {
        {1U, {0, 1, 0}},
        {2U, {1, 0, 0}},
    };
    const auto plan =
        iramix::audio::compileRenderPlan(graph, info);
    require(plan.valid, "swap plan compiles");

    iramix::audio::RenderPlanExecutor executor;
    auto output = std::make_shared<OutputNode>();
    std::string error;

    const auto publish = [&](const int value) {
        auto source = std::make_shared<ConstantNode>(
            static_cast<float>(value)
        );
        const bool published = executor.prepareAndPublish(
            plan,
            {
                .maximumBlockSize = blockSize,
                .maximumMidiEventsPerNode = 8,
                .maximumMidiBytesPerNode = 32,
                .outputNode = 2U,
                .outputChannelCount = 1,
            },
            [source = std::move(source), output](
                const iramix::audio::NodeId id
            ) -> std::shared_ptr<iramix::audio::IAudioNode> {
                if (id == 1U) {
                    return source;
                }
                if (id == 2U) {
                    return output;
                }
                return {};
            },
            error
        );
        require(published, error.c_str());
    };

    publish(0);
    iramix::realtime::resetAuditCounters();
    std::atomic<bool> running {true};
    const iramix::audio::TransportSnapshot transport {
        .playing = true,
    };
    std::thread audioThread([&] {
        while (running.load(std::memory_order_acquire)) {
            executor.process(blockSize, transport);
        }
    });

    for (int index = 1; index <= replacementCount; ++index) {
        publish(index);
        if ((index % 8) == 0) {
            static_cast<void>(executor.reclaimRetiredPlans());
            std::this_thread::yield();
        }
    }

    for (int spin = 0;
         spin < 1'000'000
            && executor.acknowledgedGeneration()
                < executor.generation();
         ++spin) {
        std::this_thread::yield();
    }
    running.store(false, std::memory_order_release);
    audioThread.join();

    // The control side intentionally does not consume telemetry in this test.
    // Render enough deterministic blocks to prove that saturation remains
    // bounded and is reported instead of ever blocking the callback.
    for (std::size_t index = 0U; index < 2'050U; ++index) {
        executor.process(blockSize, transport);
    }
    static_cast<void>(executor.reclaimRetiredPlans());
    const auto audit = iramix::realtime::auditSnapshot();

    require(
        executor.acknowledgedGeneration() == executor.generation(),
        "audio thread acknowledges the final generation"
    );
    require(
        executor.retiredPlanCount() == 0,
        "all retired plans are reclaimed"
    );
    require(
        executor.totalReclaimedPlanCount() == replacementCount,
        "every superseded plan is reclaimed exactly once"
    );
    require(
        executor.observedSwapCount() > 1U,
        "audio thread observes live plan replacement"
    );
    require(
        executor.useAfterFreeCount() == 0,
        "no use-after-free is observed"
    );
    require(audit.allocations == 0U, "zero callback allocations");
    require(audit.deallocations == 0U, "zero callback deallocations");
    require(audit.blockingLocks == 0U, "zero callback blocking locks");
    require(
        executor.droppedTelemetryCount() > 0U,
        "telemetry saturation is bounded and counted"
    );

    const float* const rendered = executor.outputChannel(0);
    require(rendered != nullptr, "final output exists");
    require(
        std::abs(rendered[0] - static_cast<float>(replacementCount))
            < 0.000001F,
        "final generation renders"
    );

    std::cout
        << "Plan swap stress: publications="
        << replacementCount + 1
        << ", blocks=" << executor.renderedBlockCount()
        << ", observed_swaps=" << executor.observedSwapCount()
        << ", reclaimed=" << executor.totalReclaimedPlanCount()
        << ", retired=" << executor.retiredPlanCount()
        << ", allocations=" << audit.allocations
        << ", deallocations=" << audit.deallocations
        << ", blocking_locks=" << audit.blockingLocks
        << ", use_after_free=" << executor.useAfterFreeCount()
        << ", telemetry_dropped="
        << executor.droppedTelemetryCount()
        << '\n';

    return EXIT_SUCCESS;
}
