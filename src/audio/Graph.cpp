#include "iramix/audio/Graph.hpp"

#include <iterator>
#include <tuple>

namespace iramix::audio {

bool GraphConnection::operator<(
    const GraphConnection& other
) const noexcept {
    return std::tie(
        sourceNode,
        sourceChannel,
        destinationNode,
        destinationChannel
    ) < std::tie(
        other.sourceNode,
        other.sourceChannel,
        other.destinationNode,
        other.destinationChannel
    );
}

void GraphDescription::clear() noexcept {
    nodes_.clear();
    connections_.clear();
}

bool GraphDescription::addNode(const NodeId id) {
    return id != 0U && nodes_.insert(id).second;
}

bool GraphDescription::removeNode(const NodeId id) {
    if (nodes_.erase(id) == 0U) {
        return false;
    }

    for (auto iterator = connections_.begin();
         iterator != connections_.end();) {
        if (iterator->sourceNode == id
            || iterator->destinationNode == id) {
            iterator = connections_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return true;
}

bool GraphDescription::addConnection(
    const GraphConnection& connection
) {
    if (connection.sourceNode == connection.destinationNode
        || connection.hasMixedLaneTypes()
        || (!connection.isMidi()
            && (connection.sourceChannel < 0
                || connection.destinationChannel < 0))
        || !containsNode(connection.sourceNode)
        || !containsNode(connection.destinationNode)) {
        return false;
    }

    return connections_.insert(connection).second;
}

bool GraphDescription::removeConnection(
    const GraphConnection& connection
) {
    return connections_.erase(connection) > 0U;
}

bool GraphDescription::containsNode(const NodeId id) const {
    return nodes_.contains(id);
}

bool GraphDescription::containsConnection(
    const GraphConnection& connection
) const {
    return connections_.contains(connection);
}

std::vector<NodeId> GraphDescription::nodes() const {
    return {nodes_.begin(), nodes_.end()};
}

std::vector<GraphConnection> GraphDescription::connections() const {
    return {connections_.begin(), connections_.end()};
}

} // namespace iramix::audio
