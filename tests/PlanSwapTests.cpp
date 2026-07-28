#include "iramix/audio/Graph.hpp"
#include "iramix/audio/Node.hpp"
#include "iramix/audio/RenderPlan.hpp"
#include "iramix/audio/RenderPlanExecutor.hpp"
#include "iramix/realtime/Audit.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
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

class PassThroughNode final : public iramix::audio::IAudioNode {
public:
    void prepare(
        const iramix::audio::NodePrepareInfo&
    ) override {}

    void process(
        const iramix::audio::NodeProcessContext&
    ) noexcept override {}
};

struct EditVariant final {
    iramix::audio::RenderPlan plan;
    std::map<
        iramix::audio::NodeId,
        std::shared_ptr<iramix::audio::IAudioNode>
    > nodes;
};

EditVariant makeEditVariant(
    const int editIndex,
    const std::shared_ptr<OutputNode>& output
) {
    constexpr iramix::audio::NodeId sourceA = 1U;
    constexpr iramix::audio::NodeId sourceB = 2U;
    constexpr iramix::audio::NodeId processorA = 10U;
    constexpr iramix::audio::NodeId processorB = 20U;
    constexpr iramix::audio::NodeId outputId = 100U;

    EditVariant variant;
    iramix::audio::GraphDescription graph;
    iramix::audio::NodeInfoMap info;

    const auto addNode = [&](const iramix::audio::NodeId id,
                             const iramix::audio::NodeInfo nodeInfo,
                             std::shared_ptr<
                                 iramix::audio::IAudioNode
                             > node) {
        require(graph.addNode(id), "edit-storm node");
        info.emplace(id, nodeInfo);
        variant.nodes.emplace(id, std::move(node));
    };
    const auto connect = [&](
        const iramix::audio::NodeId source,
        const iramix::audio::NodeId destination
    ) {
        require(
            graph.addConnection({source, 0, destination, 0}),
            "edit-storm connection"
        );
    };

    addNode(
        sourceA,
        {0, 1, 0},
        std::make_shared<ConstantNode>(
            static_cast<float>(editIndex)
        )
    );
    addNode(outputId, {1, 0, 0}, output);

    switch (editIndex % 5) {
    case 0:
        connect(sourceA, outputId);
        break;
    case 1:
        addNode(
            processorA,
            {1, 1, 0},
            std::make_shared<PassThroughNode>()
        );
        connect(sourceA, processorA);
        connect(processorA, outputId);
        break;
    case 2:
        addNode(
            sourceB,
            {0, 1, 0},
            std::make_shared<ConstantNode>(0.0F)
        );
        connect(sourceA, outputId);
        connect(sourceB, outputId);
        break;
    case 3:
        addNode(
            processorA,
            {1, 1, 17},
            std::make_shared<PassThroughNode>()
        );
        addNode(
            processorB,
            {1, 1, 0},
            std::make_shared<PassThroughNode>()
        );
        addNode(
            sourceB,
            {0, 1, 0},
            std::make_shared<ConstantNode>(0.0F)
        );
        connect(sourceA, processorA);
        connect(sourceB, processorB);
        connect(processorA, outputId);
        connect(processorB, outputId);
        break;
    default:
        addNode(
            processorA,
            {1, 1, 0},
            std::make_shared<PassThroughNode>()
        );
        addNode(
            processorB,
            {1, 1, 0},
            std::make_shared<PassThroughNode>()
        );
        connect(sourceA, processorA);
        connect(processorA, processorB);
        connect(processorB, outputId);
        break;
    }

    variant.plan = iramix::audio::compileRenderPlan(graph, info);
    require(variant.plan.valid, variant.plan.error.c_str());
    return variant;
}

} // namespace

int main() {
    constexpr int replacementCount = 5'000;
    constexpr int blockSize = 64;

    iramix::audio::RenderPlanExecutor executor;
    auto output = std::make_shared<OutputNode>();
    std::string error;

    const auto publish = [&](const int value) {
        auto variant = makeEditVariant(value, output);
        const bool published = executor.prepareAndPublish(
            variant.plan,
            {
                .maximumBlockSize = blockSize,
                .maximumMidiEventsPerNode = 8,
                .maximumMidiBytesPerNode = 32,
                .outputNode = 100U,
                .outputChannelCount = 1,
            },
            [nodes = std::move(variant.nodes)](
                const iramix::audio::NodeId id
            ) -> std::shared_ptr<iramix::audio::IAudioNode> {
                const auto iterator = nodes.find(id);
                return iterator != nodes.end()
                    ? iterator->second
                    : nullptr;
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
        << "Graph edit storm: variants=5, publications="
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
