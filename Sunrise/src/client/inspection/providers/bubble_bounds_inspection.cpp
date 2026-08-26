#include "bubble_bounds_inspection.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

namespace sunrise::client::inspection::providers::bubble_bounds {
namespace {

State g_state{};

[[nodiscard]] std::string bubble_label(std::uint64_t tag, const std::string& family) {
    std::array<char, 64> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "Bubble 0x%08llX  %s",
                                      static_cast<unsigned long long>(tag),
                                      family.c_str());
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Bubble");
}

[[nodiscard]] std::string active_family(const Source& source) {
    constexpr std::string_view kDestinationSuffix = "_destination";
    constexpr std::string_view kFreeroamSuffix = "_freeroam";
    const auto without_suffix = [](std::string value, std::string_view suffix) {
        if (value.size() > suffix.size() && std::string_view(value).ends_with(suffix)) {
            value.resize(value.size() - suffix.size());
        }
        return value;
    };
    if (!source.mapStem.empty()) {
        return without_suffix(source.mapStem, kDestinationSuffix);
    }
    return without_suffix(source.packageName, kFreeroamSuffix);
}

} // namespace

const State& state() noexcept {
    return g_state;
}

bool activate_location(bubble_catalog::Catalog location, std::string_view family) noexcept {
    try {
        std::string error;
        if (family.empty() || location.contentBuild != bubble_catalog::kTargetContentBuild
            || !bubble_catalog::validate(location, error)
            || !std::ranges::all_of(location.bubbles, [family](const auto& bubble) {
                   return bubble.family == family;
               })) {
            return false;
        }
        g_state.locationCatalog = std::move(location);
        g_state.locationFamily = std::string(family);
        g_state.locationActive = true;
        ++g_state.publicationRevision;
        if (g_state.publicationRevision == 0) {
            g_state.publicationRevision = 1;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void deactivate_location() noexcept {
    if (!g_state.locationActive) {
        return;
    }
    g_state.locationCatalog = {};
    g_state.locationFamily.clear();
    g_state.locationActive = false;
    ++g_state.publicationRevision;
    if (g_state.publicationRevision == 0) {
        g_state.publicationRevision = 1;
    }
}

std::uint64_t publication_revision() noexcept {
    return g_state.publicationRevision;
}

AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent) {
    AppendResult result{};
    const std::string family = active_family(source);
    const bool locationMatch = g_state.locationActive && g_state.locationFamily == family;
    result.present = locationMatch;
    result.diagnostic = locationMatch ? "Current-location bubble-bounds cache loaded."
                                      : "No matching current-location bubble bounds are active.";
    if (!result.present) {
        return result;
    }
    const bubble_catalog::Catalog& evidence = g_state.locationCatalog;
    result.contentBuild = evidence.contentBuild;

    if (family.empty()) {
        diagnostics.push_back({Diagnostic::Severity::information,
                               "Bubble bounds are active, but the current map family is "
                               "unavailable."});
        return result;
    }
    const bool hasFamily = std::ranges::any_of(
        evidence.bubbles,
        [&family](const bubble_catalog::Bubble& bubble) { return bubble.family == family; });
    if (!hasFamily) {
        diagnostics.push_back(
            {Diagnostic::Severity::information,
             "The bubble-bounds catalog has no rows for active family " + family + "."});
        return result;
    }

    Node groupNode;
    groupNode.name = "Map bubbles · " + family;
    groupNode.searchText = "map bubble bounds aabb package footprint catalog";
    groupNode.kind = NodeKind::geometry;
    groupNode.status = Status::known;
    groupNode.producer = Producer::catalog;
    groupNode.provenance = Provenance::catalog;
    groupNode.nativeKey = result.contentBuild;
    groupNode.source = source;
    groupNode.actions = Action::copyId;
    result.groupNode = graph.add(std::move(groupNode), parent);
    if (!result.groupNode) {
        diagnostics.push_back({Diagnostic::Severity::error,
                               "The inspection graph could not create the bubble group node."});
        return result;
    }

    for (const bubble_catalog::Bubble& bubble : evidence.bubbles) {
        if (bubble.family != family) {
            continue;
        }
        Node node;
        node.name = bubble_label(bubble.tag, bubble.family);
        node.searchText = "map bubble bounds aabb package footprint catalog";
        node.kind = NodeKind::geometry;
        node.status = Status::known;
        node.producer = Producer::catalog;
        node.provenance = Provenance::catalog;
        node.nativeKey = bubble.tag;
        node.source = source;
        node.bounds = Bounds{bubble.minimum, bubble.maximum};
        node.boundsProvenance = Provenance::catalog;
        node.transform = Transform{(bubble.minimum[0] + bubble.maximum[0]) * 0.5F,
                                   (bubble.minimum[1] + bubble.maximum[1]) * 0.5F,
                                   (bubble.minimum[2] + bubble.maximum[2]) * 0.5F};
        // Bubble catalog rows include spaces that are not currently streamed.
        // Never drive the native camera to an aggregate/offline bounds center.
        node.actions = Action::hide | Action::isolate | Action::copyId | Action::copyPosition;
        if (!graph.add(std::move(node), result.groupNode)) {
            diagnostics.push_back(
                {Diagnostic::Severity::error,
                 "The inspection graph reached its node-id capacity for map bubbles."});
            break;
        }
        ++result.bubbleCount;
    }
    return result;
}

} // namespace sunrise::client::inspection::providers::bubble_bounds
