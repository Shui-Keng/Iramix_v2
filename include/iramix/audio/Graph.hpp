#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace iramix::audio {

using NodeId = std::uint32_t;
inline constexpr int kMidiLane = -1;

struct GraphConnection final {
    NodeId sourceNode {0U};
    int sourceChannel {0};
    NodeId destinationNode {0U};
    int destinationChannel {0};

    [[nodiscard]] bool isMidi() const noexcept {
        return sourceChannel == kMidiLane
            && destinationChannel == kMidiLane;
    }

    [[nodiscard]] bool hasMixedLaneTypes() const noexcept {
        return (sourceChannel == kMidiLane)
            != (destinationChannel == kMidiLane);
    }

    [[nodiscard]] bool operator<(
        const GraphConnection& other
    ) const noexcept;

    [[nodiscard]] bool operator==(
        const GraphConnection& other
    ) const noexcept = default;
};

// Mutable control-thread topology. The audio callback sees only a compiled,
// immutable RenderPlan.
class GraphDescription final {
public:
    void clear() noexcept;
    [[nodiscard]] bool addNode(NodeId id);
    [[nodiscard]] bool removeNode(NodeId id);
    [[nodiscard]] bool addConnection(const GraphConnection& connection);
    [[nodiscard]] bool removeConnection(const GraphConnection& connection);

    [[nodiscard]] bool containsNode(NodeId id) const;
    [[nodiscard]] bool containsConnection(
        const GraphConnection& connection
    ) const;

    [[nodiscard]] std::size_t nodeCount() const noexcept {
        return nodes_.size();
    }

    [[nodiscard]] std::size_t connectionCount() const noexcept {
        return connections_.size();
    }

    [[nodiscard]] std::vector<NodeId> nodes() const;
    [[nodiscard]] std::vector<GraphConnection> connections() const;

private:
    std::set<NodeId> nodes_;
    std::set<GraphConnection> connections_;
};

} // namespace iramix::audio
