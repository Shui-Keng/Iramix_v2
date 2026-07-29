#include "iramix/plugin/PluginStateAutosave.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace iramix::plugin {

persistence::ImmutableSessionSnapshot captureLivePluginState(
    persistence::SessionDocument document,
    const std::span<const LivePluginBinding> liveBindings,
    PluginStateCaptureReport& report
) {
    report = {};
    for (auto& sessionPlugin : document.plugins) {
        const auto binding = std::find_if(
            liveBindings.begin(),
            liveBindings.end(),
            [&sessionPlugin](const LivePluginBinding& candidate) {
                return candidate.stableId == sessionPlugin.stableId;
            }
        );
        if (binding == liveBindings.end() || binding->bridge == nullptr) {
            ++report.unchanged;
            continue;
        }
        std::vector<std::byte> captured;
        if (binding->bridge->captureState(captured)
            == PluginStateStatus::ok) {
            sessionPlugin.state = std::move(captured);
            ++report.captured;
        } else {
            ++report.failed;
        }
    }
    return std::make_shared<const persistence::SessionDocument>(
        std::move(document)
    );
}

} // namespace iramix::plugin
