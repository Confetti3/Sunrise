#include "world_inspection_model.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <limits>
#include <cmath>

namespace sunrise::client::inspection {
namespace {

[[nodiscard]] std::string normalize(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const unsigned char value : text) {
        result.push_back(static_cast<char>(std::tolower(value)));
    }
    return result;
}

void append_search(std::string& target, std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (!target.empty()) {
        target.push_back(' ');
    }
    target.append(value);
}

void append_hex(std::string& target, std::uint64_t value, int width) {
    std::array<char, 32> text{};
    const int written = std::snprintf(text.data(), text.size(), "0x%0*llx", width,
                                      static_cast<unsigned long long>(value));
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        append_search(target, std::string_view(text.data(), static_cast<std::size_t>(written)));
        append_search(target, std::string_view(text.data() + 2,
                                               static_cast<std::size_t>(written - 2)));
    }
}

void append_decimal(std::string& target, std::uint64_t value) {
    std::array<char, 32> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
    if (converted.ec == std::errc{}) {
        append_search(target,
                      std::string_view(text.data(), static_cast<std::size_t>(converted.ptr - text.data())));
    }
}

[[nodiscard]] bool parse_number(std::string_view text,
                                int defaultBase,
                                std::uint64_t& output) noexcept {
    if (text.empty()) {
        return false;
    }
    int base = defaultBase;
    if (text.size() > 2 && text[0] == '0' && text[1] == 'x') {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) {
        return false;
    }
    output = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), output, base);
    return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
}

[[nodiscard]] QueryTerm::Field field_for(std::string_view name) noexcept {
    if (name == "type") {
        return QueryTerm::Field::type;
    }
    if (name == "world") {
        return QueryTerm::Field::world;
    }
    if (name == "tag") {
        return QueryTerm::Field::tag;
    }
    if (name == "class") {
        return QueryTerm::Field::classHash;
    }
    if (name == "status") {
        return QueryTerm::Field::status;
    }
    return QueryTerm::Field::any;
}

[[nodiscard]] bool field_matches(const Node& node, const QueryTerm& term) noexcept {
    switch (term.field) {
    case QueryTerm::Field::any:
        return node.searchText.find(term.value) != std::string::npos;
    case QueryTerm::Field::type:
        return normalize(kind_name(node.kind)).find(term.value) != std::string::npos;
    case QueryTerm::Field::status:
        return normalize(status_name(node.status)).find(term.value) != std::string::npos;
    case QueryTerm::Field::world: {
        std::uint64_t value = 0;
        return node.worldId.has_value() && parse_number(term.value, 10, value)
               && *node.worldId == value;
    }
    case QueryTerm::Field::tag: {
        std::uint64_t value = 0;
        return node.tag.has_value() && parse_number(term.value, 16, value)
               && *node.tag == value;
    }
    case QueryTerm::Field::classHash: {
        std::uint64_t value = 0;
        return node.classHash.has_value() && parse_number(term.value, 16, value)
               && *node.classHash == value;
    }
    }
    return false;
}

} // namespace

bool bounds_valid(const Bounds& bounds) noexcept {
    for (std::size_t lane = 0; lane < bounds.minimum.size(); ++lane) {
        if (!std::isfinite(bounds.minimum[lane]) || !std::isfinite(bounds.maximum[lane])
            || bounds.minimum[lane] > bounds.maximum[lane]) {
            return false;
        }
    }
    return true;
}

std::array<float, 3> bounds_center(const Bounds& bounds) noexcept {
    return {(bounds.minimum[0] + bounds.maximum[0]) * 0.5F,
            (bounds.minimum[1] + bounds.maximum[1]) * 0.5F,
            (bounds.minimum[2] + bounds.maximum[2]) * 0.5F};
}

std::array<float, 3> bounds_extents(const Bounds& bounds) noexcept {
    return {(bounds.maximum[0] - bounds.minimum[0]) * 0.5F,
            (bounds.maximum[1] - bounds.minimum[1]) * 0.5F,
            (bounds.maximum[2] - bounds.minimum[2]) * 0.5F};
}

std::array<std::array<float, 3>, 8> bounds_corners(const Bounds& bounds) noexcept {
    return {{{bounds.minimum[0], bounds.minimum[1], bounds.minimum[2]},
             {bounds.maximum[0], bounds.minimum[1], bounds.minimum[2]},
             {bounds.maximum[0], bounds.maximum[1], bounds.minimum[2]},
             {bounds.minimum[0], bounds.maximum[1], bounds.minimum[2]},
             {bounds.minimum[0], bounds.minimum[1], bounds.maximum[2]},
             {bounds.maximum[0], bounds.minimum[1], bounds.maximum[2]},
             {bounds.maximum[0], bounds.maximum[1], bounds.maximum[2]},
             {bounds.minimum[0], bounds.maximum[1], bounds.maximum[2]}}};
}

void Graph::reset(std::uint32_t generation) {
    nodes_.clear();
    root_ = {};
    generation_ = generation == 0 ? 1 : generation;
}

NodeId Graph::id_for_index(std::size_t index) const noexcept {
    if (index >= (std::numeric_limits<std::uint32_t>::max)()) {
        return {};
    }
    return NodeId{(static_cast<std::uint64_t>(generation_) << 32U)
                  | (static_cast<std::uint64_t>(index) + 1U)};
}

std::optional<std::size_t> Graph::index_for_id(NodeId id) const noexcept {
    const std::uint32_t generation = static_cast<std::uint32_t>(id.value >> 32U);
    const std::uint32_t encoded = static_cast<std::uint32_t>(id.value);
    if (id.value == 0 || generation != generation_ || encoded == 0) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(encoded - 1U);
    if (index >= nodes_.size()) {
        return std::nullopt;
    }
    return index;
}

NodeId Graph::add(Node node, NodeId parent) {
    if (node.bounds.has_value()) {
        if (!bounds_valid(*node.bounds)) {
            return {};
        }
        if (!node.boundsProvenance.has_value()) {
            node.boundsProvenance = node.provenance;
        }
    } else {
        node.boundsProvenance.reset();
    }
    if (parent && this->node(parent) == nullptr) {
        return {};
    }
    const NodeId id = id_for_index(nodes_.size());
    if (!id) {
        return {};
    }
    node.id = id;
    node.parent = parent;
    node.searchText = normalize(node.searchText.empty() ? node.name : node.searchText);
    append_search(node.searchText, normalize(kind_name(node.kind)));
    append_search(node.searchText, normalize(status_name(node.status)));
    if (node.tag.has_value()) {
        append_hex(node.searchText, *node.tag, 8);
    }
    if (node.classHash.has_value()) {
        append_hex(node.searchText, *node.classHash, 8);
    }
    if (node.worldId.has_value()) {
        append_decimal(node.searchText, *node.worldId);
        append_hex(node.searchText, *node.worldId, 16);
    }
    if (node.nameHash.has_value()) {
        append_hex(node.searchText, *node.nameHash, 8);
    }
    append_search(node.searchText, normalize(node.source.packageName));
    append_search(node.searchText, normalize(node.source.mapStem));
    if (node.source.scenarioTag.has_value()) {
        append_hex(node.searchText, *node.source.scenarioTag, 8);
    }
    if (node.source.spawnSetHash.has_value()) {
        append_hex(node.searchText, *node.source.spawnSetHash, 8);
    }
    if (node.source.activitySession.has_value()) {
        append_decimal(node.searchText, *node.source.activitySession);
        append_hex(node.searchText, *node.source.activitySession, 16);
    }
    if (node.activityMetadata.has_value()) {
        append_hex(node.searchText, node.activityMetadata->activityHash, 8);
        append_hex(node.searchText, node.activityMetadata->graphHash, 8);
        append_hex(node.searchText, node.activityMetadata->nodeHash, 8);
        append_search(node.searchText, node.activityMetadata->catalogVersion);
    }

    nodes_.push_back(std::move(node));
    if (parent) {
        Node* parentNode = this->node(parent);
        if (parentNode != nullptr) {
            parentNode->children.push_back(id);
        }
    } else if (!root_) {
        root_ = id;
    }
    return id;
}

const Node* Graph::node(NodeId id) const noexcept {
    const std::optional<std::size_t> index = index_for_id(id);
    return index.has_value() ? &nodes_[*index] : nullptr;
}

Node* Graph::node(NodeId id) noexcept {
    const std::optional<std::size_t> index = index_for_id(id);
    return index.has_value() ? &nodes_[*index] : nullptr;
}

const std::vector<Node>& Graph::nodes() const noexcept {
    return nodes_;
}

NodeId Graph::root() const noexcept {
    return root_;
}

std::uint32_t Graph::generation() const noexcept {
    return generation_;
}

std::string Graph::breadcrumb(NodeId id) const {
    std::vector<std::string_view> labels;
    labels.reserve(8);
    NodeId cursor = id;
    while (cursor && labels.size() <= nodes_.size()) {
        const Node* current = node(cursor);
        if (current == nullptr) {
            break;
        }
        labels.push_back(current->name);
        cursor = current->parent;
    }
    std::string result;
    for (auto iterator = labels.rbegin(); iterator != labels.rend(); ++iterator) {
        if (!result.empty()) {
            result.append(" / ");
        }
        result.append(*iterator);
    }
    return result;
}

Query parse_query(std::string_view text) {
    Query result;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text.size()) {
            break;
        }

        std::string token;
        if (text[cursor] == '"') {
            ++cursor;
            const std::size_t start = cursor;
            while (cursor < text.size() && text[cursor] != '"') {
                ++cursor;
            }
            token = normalize(text.substr(start, cursor - start));
            if (cursor < text.size()) {
                ++cursor;
            }
        } else {
            const std::size_t start = cursor;
            while (cursor < text.size()
                   && std::isspace(static_cast<unsigned char>(text[cursor])) == 0) {
                ++cursor;
            }
            token = normalize(text.substr(start, cursor - start));
        }
        if (token.empty()) {
            continue;
        }

        QueryTerm term;
        const std::size_t separator = token.find(':');
        if (separator != std::string::npos && separator != 0 && separator + 1 < token.size()) {
            const QueryTerm::Field field = field_for(std::string_view(token).substr(0, separator));
            if (field != QueryTerm::Field::any) {
                term.field = field;
                term.value = token.substr(separator + 1);
                result.terms.push_back(std::move(term));
                continue;
            }
        }
        term.value = std::move(token);
        result.terms.push_back(std::move(term));
    }
    return result;
}

bool matches(const Node& node, const Query& query) noexcept {
    return std::ranges::all_of(query.terms,
                               [&node](const QueryTerm& term) { return field_matches(node, term); });
}

const char* kind_name(NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::world:
        return "World";
    case NodeKind::source:
        return "Source";
    case NodeKind::activity:
        return "Activity";
    case NodeKind::activityGraph:
        return "Activity Graph";
    case NodeKind::activityGraphNode:
        return "Activity Graph Node";
    case NodeKind::activityReference:
        return "Activity Reference";
    case NodeKind::destination:
        return "Destination";
    case NodeKind::spawnGroup:
        return "Spawns";
    case NodeKind::spawnSet:
        return "Spawn Set";
    case NodeKind::spawnPoint:
        return "Spawn Point";
    case NodeKind::geometry:
        return "Geometry";
    case NodeKind::terrain:
        return "Terrain";
    case NodeKind::runtimeEntity:
        return "Runtime Entity";
    case NodeKind::placedObject:
        return "Placed Object";
    case NodeKind::componentSlot:
        return "Component Slot";
    case NodeKind::light:
        return "Light";
    case NodeKind::trigger:
        return "Trigger";
    case NodeKind::audio:
        return "Audio";
    case NodeKind::physics:
        return "Physics";
    case NodeKind::navigation:
        return "Navigation";
    case NodeKind::unresolved:
        return "Unresolved";
    }
    return "Unknown";
}

const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::known:
        return "Known";
    case Status::unknownSemantic:
        return "Unknown semantic";
    case Status::deferred:
        return "Deferred";
    case Status::failed:
        return "Failed";
    }
    return "Unknown";
}

const char* producer_name(Producer producer) noexcept {
    switch (producer) {
    case Producer::graph: return "graph";
    case Producer::catalog: return "catalog";
    case Producer::activityCatalog: return "activity-catalog";
    case Producer::localPlayer: return "local-player";
    case Producer::objectSystem: return "object-system";
    case Producer::trigger: return "trigger";
    case Producer::audioListener: return "audio-listener";
    case Producer::physics: return "physics";
    case Producer::audioEmitter: return "audio-emitter";
    case Producer::navigation: return "navigation";
    case Producer::light: return "light";
    case Producer::terrain: return "terrain";
    }
    return "graph";
}

const char* provenance_name(Provenance provenance) noexcept {
    switch (provenance) {
    case Provenance::derived: return "derived";
    case Provenance::catalog: return "catalog";
    case Provenance::runtime: return "runtime";
    }
    return "derived";
}

void Selection::reconcile(const Graph& graph) noexcept {
    if (selected_ && graph.node(selected_) == nullptr) {
        selected_ = {};
    }
    if (hovered_ && graph.node(hovered_) == nullptr) {
        hovered_ = {};
    }
}

void Selection::select(NodeId id) noexcept {
    selected_ = id;
}

void Selection::hover(NodeId id) noexcept {
    hovered_ = id;
}

void Selection::clear() noexcept {
    selected_ = {};
    hovered_ = {};
}

void Selection::clear_hover() noexcept {
    hovered_ = {};
}

NodeId Selection::selected() const noexcept {
    return selected_;
}

NodeId Selection::hovered() const noexcept {
    return hovered_;
}

} // namespace sunrise::client::inspection
