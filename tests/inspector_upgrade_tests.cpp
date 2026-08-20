#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_set>
#include <utility>

#include "client/inspection/providers/runtime_observation_inspection.h"
#include "client/inspection/activity_graph_catalog.h"
#include "client/inspection/world_inspection_model.h"
#include "client/ui/world_inspector/world_debug_primitives.h"
#include "client/ui/world_inspector/world_inspector_graph_layout.h"
namespace observed = sunrise::client::inspection::providers::runtime_observations;
namespace objects = sunrise::client::viewer::objects;
namespace triggers = sunrise::client::viewer::triggers;

namespace inspection = sunrise::client::inspection;
namespace debug = sunrise::client::ui::world_inspector::debug_primitives;
namespace graph_layout = sunrise::client::ui::world_inspector::graph_layout;
namespace catalog = sunrise::client::inspection::activity_catalog;

[[nodiscard]] bool require(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
    }
    return condition;
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    bytes[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    bytes[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

void write_float(std::vector<std::byte>& bytes, std::size_t offset, float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    write_u32(bytes, offset, bits);
}

[[nodiscard]] std::vector<std::byte> minimal_catalog() {
    constexpr std::size_t header = catalog::kHeaderSize;
    constexpr std::size_t activity = header;
    constexpr std::size_t graph = activity + 20;
    constexpr std::size_t node = graph + 20;
    constexpr std::size_t activityRef = node + 40;
    constexpr std::size_t linkedRef = activityRef + 4;
    constexpr std::size_t strings = linkedRef;
    constexpr std::size_t total = strings + 7;
    std::vector<std::byte> bytes(total);
    constexpr std::array<char, 8> magic{'S', 'A', 'C', 'A', 'T', '0', '0', '1'};
    for (std::size_t index = 0; index < magic.size(); ++index) {
        bytes[index] = static_cast<std::byte>(magic[index]);
    }
    write_u32(bytes, 8, 1);
    write_u32(bytes, 12, 86657);
    write_u32(bytes, 16, header);
    write_u32(bytes, 20, total);
    write_u32(bytes, 24, static_cast<std::uint32_t>(strings));
    write_u32(bytes, 28, 1);
    write_u32(bytes, 32, static_cast<std::uint32_t>(strings));
    write_u32(bytes, 36, 7);
    const std::array<std::array<std::uint32_t, 3>, 6> sections{
        std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(activity), 1, 20},
        std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(graph), 1, 20},
        std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(node), 1, 40},
        std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(activityRef), 1, 4},
        std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(linkedRef), 0, 4},
        std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(linkedRef), 0, 40}};
    std::size_t sectionOffset = 40;
    for (const auto& section : sections) {
        write_u32(bytes, sectionOffset, section[0]);
        write_u32(bytes, sectionOffset + 4, section[1]);
        write_u32(bytes, sectionOffset + 8, section[2]);
        sectionOffset += 12;
    }
    std::memcpy(bytes.data() + strings, "v\0Name\0", 7);
    write_u32(bytes, activity, 2);
    write_u32(bytes, activity + 4, static_cast<std::uint32_t>(strings + 2));
    write_u32(bytes, activity + 8, 4);
    write_u32(bytes, activity + 12, 0);
    write_u32(bytes, activity + 16, 1);
    write_u32(bytes, graph, 2);
    write_u32(bytes, graph + 4, 0);
    write_u32(bytes, graph + 8, 1);
    write_u32(bytes, graph + 12, 0);
    write_u32(bytes, graph + 16, 0);
    write_u32(bytes, node, 2);
    write_u32(bytes, node + 4, 3);
    write_float(bytes, node + 8, 4.0F);
    write_float(bytes, node + 12, 5.0F);
    write_u32(bytes, node + 16, 6);
    write_u32(bytes, node + 20, 7);
    write_u32(bytes, node + 24, 0);
    write_u32(bytes, node + 28, 1);
    write_u32(bytes, node + 32, 0);
    write_u32(bytes, node + 36, 0);
    write_u32(bytes, activityRef, 2);
    return bytes;
}

int main() {
    inspection::Bounds valid{{-1.0F, -2.0F, 0.0F}, {3.0F, 4.0F, 8.0F}};
    if (!require(inspection::bounds_valid(valid), "valid bounds")
        || !require(inspection::bounds_center(valid) == std::array<float, 3>{1.0F, 1.0F, 4.0F},
                    "bounds center")
        || !require(inspection::bounds_extents(valid) == std::array<float, 3>{2.0F, 3.0F, 4.0F},
                    "bounds extents")) {
        return 1;
    }
    inspection::Bounds inverted = valid;
    inverted.minimum[0] = 5.0F;
    if (!require(!inspection::bounds_valid(inverted), "inverted bounds rejected")) {
        return 2;
    }
    inspection::Bounds nonfinite = valid;
    nonfinite.maximum[1] = std::numeric_limits<float>::quiet_NaN();
    if (!require(!inspection::bounds_valid(nonfinite), "nonfinite bounds rejected")) {
        return 3;
    }
    inspection::Graph graph;
    graph.reset(7);
    inspection::Node invalid;
    invalid.bounds = inverted;
    if (!require(!graph.add(std::move(invalid)), "graph rejects invalid bounds")) {
        return 4;
    }
    inspection::Node bounded;
    bounded.provenance = inspection::Provenance::catalog;
    bounded.bounds = valid;
    const inspection::NodeId boundedId = graph.add(std::move(bounded));
    if (!require(boundedId && graph.node(boundedId)->boundsProvenance
                     == inspection::Provenance::catalog,
                 "bounds provenance defaults from producer")) {
        return 5;
    }
    observed::ObjectSnapshot objectSnapshot{};
    objectSnapshot.present = true;
    objectSnapshot.objectCount = 1;
    objectSnapshot.declaredCount = 1;
    objectSnapshot.objects[0].handle = 0x1234;
    objectSnapshot.objects[0].type = 0x01;
    objectSnapshot.objects[0].position = {7.0F, 8.0F, 9.0F};
    objectSnapshot.objects[0].positionPresent = true;
    observed::TriggerSnapshot triggerSnapshot{};
    triggerSnapshot.present = true;
    triggerSnapshot.triggerCount = 1;
    triggerSnapshot.triggers[0].kind = triggers::Kind::event;
    triggerSnapshot.triggers[0].observationId = 44;
    triggerSnapshot.triggers[0].objectHandle = 0x1234;
    inspection::Graph triggerGraph;
    triggerGraph.reset(1);
    std::vector<inspection::Diagnostic> triggerDiagnostics;
    const observed::AppendResult triggerResult =
        observed::append(triggerGraph,
                         triggerDiagnostics,
                         objectSnapshot,
                         triggerSnapshot,
                         {},
                         {},
                         {},
                         {});
    const inspection::Node* eventNode =
        triggerGraph.node(triggerResult.triggers.empty() ? inspection::NodeId{}
                                                          : triggerResult.triggers.front());
    if (!require(eventNode != nullptr && eventNode->transform.has_value()
                     && inspection::supports(eventNode->actions, inspection::Action::focus)
                     && inspection::supports(eventNode->actions, inspection::Action::copyId),
                 "event trigger matches live object position and actions")) {
        return 6;
    }
    objectSnapshot.objects[0].positionPresent = false;
    observed::update_triggers(triggerGraph,
                              triggerResult.triggers,
                              triggerSnapshot,
                              objectSnapshot);
    eventNode = triggerGraph.node(triggerResult.triggers.front());
    if (!require(eventNode != nullptr && !eventNode->transform.has_value()
                     && eventNode->actions == inspection::Action::copyId,
                 "event trigger clears stale position actions")) {
        return 7;
    }
    objectSnapshot.objects[0].positionPresent = true;
    observed::update_triggers(triggerGraph,
                              triggerResult.triggers,
                              triggerSnapshot,
                              objectSnapshot);
    eventNode = triggerGraph.node(triggerResult.triggers.front());
    if (!require(eventNode != nullptr && eventNode->transform.has_value()
                     && eventNode->transform->position == std::array<float, 3>{7.0F, 8.0F, 9.0F},
                 "event trigger position refresh")) {
        return 8;
    }
    inspection::Graph layoutGraph;
    layoutGraph.reset(3);
    inspection::Node rootNode;
    rootNode.name = "root";
    const inspection::NodeId rootId = layoutGraph.add(std::move(rootNode));
    inspection::Node branchNode;
    branchNode.name = "branch";
    const inspection::NodeId branchId = layoutGraph.add(std::move(branchNode), rootId);
    inspection::Node siblingNode;
    siblingNode.name = "sibling";
    const inspection::NodeId siblingId = layoutGraph.add(std::move(siblingNode), rootId);
    inspection::Node leafOne;
    leafOne.name = "leaf-one";
    const inspection::NodeId leafOneId = layoutGraph.add(std::move(leafOne), branchId);
    inspection::Node leafTwo;
    leafTwo.name = "leaf-two";
    const inspection::NodeId leafTwoId = layoutGraph.add(std::move(leafTwo), branchId);
    std::unordered_set<std::uint64_t> admitted{
        rootId.value, branchId.value, siblingId.value, leafOneId.value, leafTwoId.value};
    std::vector<graph_layout::LayoutNode> firstLayout;
    std::vector<graph_layout::LayoutNode> secondLayout;
    graph_layout::compute(layoutGraph, rootId, admitted, firstLayout);
    graph_layout::compute(layoutGraph, rootId, admitted, secondLayout);
    const auto layout_for = [](const std::vector<graph_layout::LayoutNode>& layout,
                               inspection::NodeId id) {
        return std::ranges::find_if(layout, [id](const graph_layout::LayoutNode& value) {
            return value.id == id;
        });
    };
    const auto firstBranch = layout_for(firstLayout, branchId);
    const auto firstLeafOne = layout_for(firstLayout, leafOneId);
    const auto firstLeafTwo = layout_for(firstLayout, leafTwoId);
    if (!require(firstLayout.size() == 5 && secondLayout == firstLayout,
                 "deterministic graph layout")
        || !require(firstBranch != firstLayout.end() && firstLeafOne != firstLayout.end()
                        && firstLeafTwo != firstLayout.end()
                        && firstBranch->position[1]
                               == (firstLeafOne->position[1] + firstLeafTwo->position[1]) * 0.5F,
                    "parent centered over admitted children")) {
        return 9;
    }
    admitted.erase(leafTwoId.value);
    graph_layout::compute(layoutGraph, rootId, admitted, firstLayout);
    if (!require(firstLayout.size() == 4, "filtered subtree layout")) {
        return 10;
    }


    debug::ProjectionContext context{};
    context.position = {0.0F, 0.0F, 0.0F};
    context.forward = {1.0F, 0.0F, 0.0F};
    context.up = {0.0F, 0.0F, 1.0F};
    context.fov = 90.0F;
    context.viewportWidth = 100.0F;
    context.viewportHeight = 100.0F;
    debug::ProjectedPoint point{};
    if (!require(debug::project_point(context, {2.0F, 0.0F, 0.0F}, point), "camera center projection")
        || !require(std::abs(point.screen.x - 50.0F) < 0.001F
                        && std::abs(point.screen.y - 50.0F) < 0.001F,
                    "camera center screen point")
        || !require(!debug::project_point(context, {-1.0F, 0.0F, 0.0F}, point),
                    "behind camera rejection")) {
        return 6;
    }
    debug::ProjectedSegment clipped{};
    if (!require(debug::project_segment(context, {-1.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, clipped),
                 "near-plane segment clipping")
        || !require(!debug::project_point(debug::ProjectionContext{context.position,
                                                                      {0.0F, 0.0F, 0.0F},
                                                                      context.up,
                                                                      context.fov,
                                                                      0.0F,
                                                                      0.0F,
                                                                      100.0F,
                                                                      100.0F,
                                                                      context.nearPlane},
                                          {2.0F, 0.0F, 0.0F},
                                          point),
                    "degenerate camera basis rejection")) {
        return 7;
    }
    inspection::Bounds front{{1.0F, -1.0F, -1.0F}, {3.0F, 1.0F, 1.0F}};
    debug::ProjectedBox box{};
    debug::ProjectedCapsule capsule{};
    const std::size_t capsuleSegments =
        debug::project_capsule(context,
                               debug::Capsule{{1.0F, 0.0F, 0.0F},
                                              {3.0F, 0.0F, 0.0F},
                                              0.5F},
                               capsule);
    if (!require(debug::project_aabb(context, front, box) == debug::kBoxEdgeCount,
                 "all twelve AABB edges")
        || !require(capsuleSegments > 0, "finite capsule tessellation")) {
        return 8;
    }

    catalog::Catalog parsed;
    std::string error;
    const std::vector<std::byte> bytes = minimal_catalog();
    const bool loaded = catalog::load(bytes, parsed, error);
    if (!loaded) {
        std::printf("catalog error: %s\n", error.c_str());
    }
    if (!require(loaded, "minimal binary catalog load")
        || !require(parsed.graphs.size() == 1 && parsed.graphs[0].nodes.size() == 1,
                    "catalog graph/node count")
        || !require(catalog::compatibility(parsed) == catalog::Compatibility::compatible,
                    "catalog build compatibility")) {
        return 9;
    }
    parsed.graphs[0].nodes[0].authoredX = std::numeric_limits<float>::infinity();
    if (!require(!catalog::validate(parsed, error), "catalog nonfinite position rejection")) {
        return 10;
    }
    parsed.contentBuild = 87221;
    if (!require(catalog::compatibility(parsed) == catalog::Compatibility::buildMismatch,
                 "catalog build mismatch is browse-only")) {
        return 11;
    }
    std::vector<std::byte> truncated = bytes;
    truncated.resize(catalog::kHeaderSize - 1U);
    catalog::Catalog malformed;
    if (!require(!catalog::load(truncated, malformed, error),
                 "truncated catalog rejection")) {
        return 12;
    }
    return 0;
}
