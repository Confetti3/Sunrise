#include "bubble_bounds_inspection.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

#include "../../../core/filesystem/path.h"

namespace sunrise::client::inspection::providers::bubble_bounds {
namespace {

constexpr std::wstring_view kCatalogSuffix = L"\\bubble-bounds-catalog.bin";
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

void initialize(void* module) noexcept {
    shutdown();
    g_state.initialized = true;
    core::path::Buffer path{};
    if (!core::path::artifact_directory(module, path)
        || !core::path::append(path, kCatalogSuffix)) {
        g_state.load.compatibility = bubble_catalog::Compatibility::missing;
        g_state.load.diagnostic = "Bubble bounds catalog artifact path is unavailable.";
        return;
    }
    g_state.load = bubble_catalog::load_file(path.chars.data(), g_state.catalog);
}

void shutdown() noexcept {
    g_state = {};
}

const State& state() noexcept {
    return g_state;
}

AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent) {
    AppendResult result{};
    result.present = g_state.load.compatibility == bubble_catalog::Compatibility::compatible
                     || g_state.load.compatibility == bubble_catalog::Compatibility::buildMismatch;
    result.buildMatch = g_state.load.compatibility == bubble_catalog::Compatibility::compatible;
    result.contentBuild = g_state.catalog.contentBuild;
    result.diagnostic = g_state.load.diagnostic;
    if (!result.present) {
        diagnostics.push_back({Diagnostic::Severity::information,
                               g_state.load.diagnostic.empty()
                                   ? "No optional bubble bounds catalog is installed."
                                   : g_state.load.diagnostic});
        return result;
    }

    const std::string family = active_family(source);
    if (family.empty()) {
        diagnostics.push_back({Diagnostic::Severity::information,
                               "Bubble bounds are installed, but the active map family is "
                               "unavailable."});
        return result;
    }
    const bool hasFamily = std::ranges::any_of(
        g_state.catalog.bubbles,
        [&family](const bubble_catalog::Bubble& bubble) { return bubble.family == family; });
    if (!hasFamily) {
        diagnostics.push_back(
            {Diagnostic::Severity::information,
             "The bubble-bounds catalog has no rows for active family " + family + "."});
        return result;
    }

    Node groupNode;
    groupNode.name = result.buildMatch ? "Map bubbles · " + family
                                       : "Map bubbles · " + family + " (Browse only)";
    groupNode.searchText = "map bubble bounds aabb package footprint catalog";
    groupNode.kind = NodeKind::geometry;
    groupNode.status = result.buildMatch ? Status::known : Status::deferred;
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

    for (const bubble_catalog::Bubble& bubble : g_state.catalog.bubbles) {
        if (bubble.family != family) {
            continue;
        }
        Node node;
        node.name = bubble_label(bubble.tag, bubble.family);
        node.searchText = "map bubble bounds aabb package footprint catalog";
        node.kind = NodeKind::geometry;
        node.status = result.buildMatch ? Status::known : Status::deferred;
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
