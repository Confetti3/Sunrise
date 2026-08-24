#include "statics_footprint_inspection.h"

#include <array>
#include <cstdio>
#include <utility>
#include <vector>

#include "../../content/statics/statics_footprints.h"

namespace sunrise::client::inspection::providers::statics_footprints {
namespace {

[[nodiscard]] std::string collection_label(std::uint32_t tag, std::uint32_t instanceCount) {
    std::array<char, 64> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "Statics 0x%08X  %u placed",
                                      tag,
                                      static_cast<unsigned>(instanceCount));
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Statics");
}

} // namespace

AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent) {
    AppendResult result{};
    if (!source.activitySession.has_value()
        || !content::statics::scope_matches(*source.activitySession)) {
        return result;
    }
    const content::statics::Progress progress = content::statics::progress();
    result.rejected = progress.rejected;
    result.truncated = progress.truncated;
    if (progress.rejected != 0 || progress.truncated != 0) {
        std::array<char, 160> text{};
        const int written = std::snprintf(
            text.data(),
            text.size(),
            "Static-footprint pass published %zu of %zu collections; %zu rejected and %zu "
            "capacity-truncated.",
            progress.published,
            progress.collections,
            progress.rejected,
            progress.truncated);
        diagnostics.push_back(
            {progress.truncated == 0 ? Diagnostic::Severity::information
                                     : Diagnostic::Severity::warning,
             written > 0 && static_cast<std::size_t>(written) < text.size()
                 ? std::string(text.data(), static_cast<std::size_t>(written))
                 : std::string("The static-footprint pass rejected incomplete rows.")});
    }
    std::vector<content::statics::Footprint> rows(content::statics::kFootprintCapacity);
    std::size_t count = 0;
    if (!content::statics::snapshot(rows, count)) {
        diagnostics.push_back({Diagnostic::Severity::warning,
                               "The static-footprint snapshot did not fit its buffer."});
        return result;
    }
    rows.resize(count);
    if (rows.empty()) {
        return result;
    }

    Node groupNode;
    groupNode.name = "Static footprints";
    groupNode.searchText = "static geometry package footprint aabb instances";
    groupNode.kind = NodeKind::geometry;
    groupNode.status = Status::known;
    groupNode.producer = Producer::catalog;
    groupNode.provenance = Provenance::catalog;
    groupNode.source = source;
    groupNode.actions = Action::copyId;
    result.groupNode = graph.add(std::move(groupNode), parent);
    if (!result.groupNode) {
        diagnostics.push_back(
            {Diagnostic::Severity::error,
             "The inspection graph could not create the static-footprint group."});
        return result;
    }
    result.present = true;

    for (const content::statics::Footprint& row : rows) {
        Node node;
        node.name = collection_label(row.tag, row.instanceCount);
        node.searchText = "static geometry package footprint aabb instances";
        node.kind = NodeKind::geometry;
        node.status = Status::known;
        node.producer = Producer::catalog;
        node.provenance = Provenance::catalog;
        node.nativeKey = row.tag;
        node.source = source;
        node.bounds = Bounds{row.minimum, row.maximum};
        node.boundsProvenance = Provenance::catalog;
        node.transform = Transform{(row.minimum[0] + row.maximum[0]) * 0.5F,
                                   (row.minimum[1] + row.maximum[1]) * 0.5F,
                                   (row.minimum[2] + row.maximum[2]) * 0.5F};
        // These package-family rows can belong to an unloaded bubble. Moving the
        // native camera to their aggregate center is unsafe; selection and copying
        // remain available while navigation stays manual.
        node.actions = Action::hide | Action::isolate | Action::copyId | Action::copyPosition;
        if (!graph.add(std::move(node), result.groupNode)) {
            diagnostics.push_back({Diagnostic::Severity::error,
                                   "The inspection graph reached its node-id capacity for static "
                                   "footprints."});
            break;
        }
        ++result.published;
    }
    return result;
}

} // namespace sunrise::client::inspection::providers::statics_footprints
