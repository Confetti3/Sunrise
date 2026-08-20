#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../../player/player_position.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::live_player {

/** Thread-safe scalar and transform snapshot published by the local-player physics observer. */
struct Snapshot final {
    std::uint32_t controlledHandle{};
    std::array<float, 3> position{};
    bool handlePresent{};
    bool positionPresent{};
};

/** Result returned after the live local-player node is appended. */
struct AppendResult final {
    NodeId node{};
};

/** Captures no gameplay pointers; the player module has already copied these fields on a game thread. */
[[nodiscard]] inline Snapshot capture() noexcept {
    const player::position::Snapshot current = player::position::snapshot();
    Snapshot result{};
    result.positionPresent = current.present;
    if (current.present) {
        result.position = current.position;
    }
    result.handlePresent = current.present && current.controlledHandlePresent;
    if (result.handlePresent) {
        result.controlledHandle = current.controlledHandle;
    }
    return result;
}

[[nodiscard]] inline std::string label(const Snapshot& snapshot) {
    std::array<char, 80> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "Local controlled object 0x%08X",
                                      snapshot.controlledHandle);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Local controlled object");
}

/** Appends the one live object identity Sunrise can currently prove without retaining game memory. */
[[nodiscard]] inline AppendResult append(Graph& graph,
                                         std::vector<Diagnostic>& diagnostics,
                                         const Snapshot& snapshot,
                                         const Source& source,
                                         NodeId parent) {
    AppendResult result{};
    if (!snapshot.handlePresent) {
        return result;
    }

    Node node;
    node.name = label(snapshot);
    node.searchText =
        "live runtime local player controlled object handle physics rigid body position";
    node.kind = NodeKind::runtimeEntity;
    node.status = snapshot.positionPresent ? Status::known : Status::deferred;
    node.source = source;
    node.runtimeEntity = snapshot.controlledHandle;
    if (snapshot.positionPresent) {
        node.transform = Transform{snapshot.position};
        node.transformRuntime = true;
    }
    node.actions = Action::focus | Action::hide | Action::isolate | Action::copyId
                   | Action::copyPosition;
    result.node = graph.add(std::move(node), parent);
    if (!result.node) {
        diagnostics.push_back(
            {Diagnostic::Severity::error,
             "The inspection graph could not create its live local-player node."});
        return result;
    }

    std::array<char, 192> text{};
    const int written = std::snprintf(
        text.data(),
        text.size(),
        "Live runtime coverage: controlled-object handle 0x%08X with physics position %s.",
        snapshot.controlledHandle,
        snapshot.positionPresent ? "available" : "deferred");
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        diagnostics.push_back(
            {Diagnostic::Severity::information,
             std::string(text.data(), static_cast<std::size_t>(written))});
    }
    return result;
}

/** Updates the copied transform without rebuilding the stable inspection graph every frame. */
inline void update(Graph& graph, NodeId nodeId, const Snapshot& snapshot) noexcept {
    Node* node = graph.node(nodeId);
    if (node == nullptr || !snapshot.handlePresent
        || node->runtimeEntity != snapshot.controlledHandle) {
        return;
    }
    node->status = snapshot.positionPresent ? Status::known : Status::deferred;
    if (snapshot.positionPresent) {
        node->transform = Transform{snapshot.position};
        node->transformRuntime = true;
    } else {
        node->transform.reset();
        node->transformRuntime = false;
    }
}

} // namespace sunrise::client::inspection::providers::live_player
