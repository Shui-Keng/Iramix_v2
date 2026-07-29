#pragma once

#include "iramix/persistence/AsyncSessionSaver.hpp"
#include "iramix/plugin/PluginBridge.hpp"

#include <cstdint>
#include <span>

namespace iramix::plugin {

// One live plugin process bound to a session's stable plugin entity. The
// binding is borrowed: the caller owns the bridge and must outlive the
// captureLivePluginState() call.
struct LivePluginBinding final {
    std::uint64_t stableId {0U};
    PluginBridge* bridge {nullptr};
};

struct PluginStateCaptureReport final {
    // Refreshed from a live bridge.
    std::uint32_t captured {0U};
    // No live binding for this session plugin; its previous state stood.
    std::uint32_t unchanged {0U};
    // A live binding exists but did not answer ok within its state
    // deadline; its previous state stood rather than being discarded.
    std::uint32_t failed {0U};
};

// Refreshes every SessionPlugin::state in `document` that has a matching
// live binding, by capture through PluginBridge::captureState(), then
// returns an immutable snapshot ready for
// SessionPersistenceService::markDirty() / requestSave().
//
// This is what closes the autosave gap the bridge's state transfer
// otherwise leaves open: captureState() alone proves a live plugin can
// hand its state back, but says nothing about that state ever reaching an
// autosaved file. A plugin with no live binding, or whose bridge fails to
// answer, keeps the state already in `document` — a stale-but-known blob
// is a safer autosave than losing the entity's state outright, and the
// report distinguishes "kept because there was nothing new" from "kept
// because the live plugin did not answer" so a caller can tell the two
// apart. Control thread only, matching PluginBridge::captureState()'s own
// contract: it does not allocate on the audio thread, but it does wait,
// bounded by each bridge's configured stateDeadline.
[[nodiscard]] persistence::ImmutableSessionSnapshot captureLivePluginState(
    persistence::SessionDocument document,
    std::span<const LivePluginBinding> liveBindings,
    PluginStateCaptureReport& report
);

} // namespace iramix::plugin
