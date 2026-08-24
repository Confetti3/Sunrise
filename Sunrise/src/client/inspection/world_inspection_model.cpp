#include "world_inspection_model.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <type_traits>

#include "inspection_descriptors.h"

namespace sunrise::client::inspection {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] std::uint64_t
hash_bytes(std::uint64_t value, const void* bytes, std::size_t size) noexcept {
    const auto* cursor = static_cast<const unsigned char*>(bytes);
    for (std::size_t index = 0; index < size; ++index) {
        value = (value ^ cursor[index]) * kFnvPrime;
    }
    return value;
}

template <typename Value>
[[nodiscard]] std::uint64_t hash_value(std::uint64_t hash, const Value& value) noexcept {
    return hash_bytes(hash, &value, sizeof(value));
}

[[nodiscard]] std::uint64_t derived_discriminator(const NodeKey& parent,
                                                  std::string_view name) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash = hash_value(hash, parent.producer);
    hash = hash_value(hash, parent.producerEpoch);
    hash = hash_value(hash, parent.kind);
    hash = hash_value(hash, parent.nativeKey);
    hash = hash_value(hash, parent.discriminator);
    hash = hash_bytes(hash, name.data(), name.size());
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] bool runtime_scoped(Producer producer) noexcept {
    return producer == Producer::localPlayer || producer == Producer::objectSystem
           || producer == Producer::trigger || producer == Producer::audioListener
           || producer == Producer::physics;
}

void append_property(Node& node, std::string key, std::string label, PropertyValue value) {
    const bool present = std::ranges::any_of(
        node.properties, [&key](const Property& property) { return property.key == key; });
    if (!present) {
        node.properties.push_back(
            {std::move(key), std::move(label), std::move(value), node.provenance});
    }
}

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
    const int written = std::snprintf(
        text.data(), text.size(), "0x%0*llx", width, static_cast<unsigned long long>(value));
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        append_search(target, std::string_view(text.data(), static_cast<std::size_t>(written)));
        append_search(target,
                      std::string_view(text.data() + 2, static_cast<std::size_t>(written - 2)));
    }
}

void append_decimal(std::string& target, std::uint64_t value) {
    std::array<char, 32> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
    if (converted.ec == std::errc{}) {
        append_search(
            target,
            std::string_view(text.data(), static_cast<std::size_t>(converted.ptr - text.data())));
    }
}

[[nodiscard]] bool
parse_number(std::string_view text, int defaultBase, std::uint64_t& output) noexcept {
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

[[nodiscard]] std::optional<QueryTerm::Field> field_for(std::string_view name) noexcept {
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
    if (name == "role") {
        return QueryTerm::Field::role;
    }
    if (name == "confidence") {
        return QueryTerm::Field::confidence;
    }
    if (name == "placement" || name == "placed") {
        return name == "placement" ? QueryTerm::Field::placement : QueryTerm::Field::placedEntity;
    }
    if (name == "relationship" || name == "linked") {
        return QueryTerm::Field::relationship;
    }
    if (name == "localized" || name == "text") {
        return QueryTerm::Field::localized;
    }
    if (name == "map" || name == "maptable") {
        return QueryTerm::Field::mapTable;
    }
    if (name == "placedentity") {
        return QueryTerm::Field::placedEntity;
    }
    return std::nullopt;
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
        return node.tag.has_value() && parse_number(term.value, 16, value) && *node.tag == value;
    }
    case QueryTerm::Field::classHash: {
        std::uint64_t value = 0;
        return node.classHash.has_value() && parse_number(term.value, 16, value)
               && *node.classHash == value;
    }
    case QueryTerm::Field::role:
        return node.activityLogicMetadata.has_value()
               && normalize(node.activityLogicMetadata->roleName).find(term.value)
                      != std::string::npos;
    case QueryTerm::Field::confidence:
        return node.activityLogicMetadata.has_value()
               && normalize(node.activityLogicMetadata->confidenceName).find(term.value)
                      != std::string::npos;
    case QueryTerm::Field::placement: {
        if (!node.activityLogicMetadata.has_value()) {
            return false;
        }
        const bool present = node.activityLogicMetadata->hasPlacement;
        return (term.value == "yes" || term.value == "true" || term.value == "1")   ? present
               : (term.value == "no" || term.value == "false" || term.value == "0") ? !present
                                                                                    : false;
    }
    case QueryTerm::Field::relationship: {
        if (!node.activityLogicMetadata.has_value()) {
            return false;
        }
        const bool present = !node.activityLogicMetadata->relationships.empty();
        return (term.value == "yes" || term.value == "true" || term.value == "1")   ? present
               : (term.value == "no" || term.value == "false" || term.value == "0") ? !present
                                                                                    : false;
    }
    case QueryTerm::Field::localized: {
        if (!node.activityLogicMetadata.has_value()) {
            return false;
        }
        const bool present = !node.activityLogicMetadata->localizedText.empty();
        return (term.value == "yes" || term.value == "true" || term.value == "1")   ? present
               : (term.value == "no" || term.value == "false" || term.value == "0") ? !present
                                                                                    : false;
    }
    case QueryTerm::Field::mapTable: {
        std::uint64_t value = 0;
        return node.activityLogicMetadata.has_value() && node.activityLogicMetadata->hasPlacement
               && parse_number(term.value, 16, value)
               && node.activityLogicMetadata->mapTableTag == value;
    }
    case QueryTerm::Field::placedEntity: {
        std::uint64_t value = 0;
        return node.activityLogicMetadata.has_value() && node.activityLogicMetadata->hasPlacement
               && parse_number(term.value, 16, value)
               && node.activityLogicMetadata->placedEntityTag == value;
    }
    }
    return false;
}

} // namespace

std::size_t NodeKeyHash::operator()(const NodeKey& key) const noexcept {
    std::uint64_t hash = kFnvOffset;
    hash = hash_value(hash, key.producer);
    hash = hash_value(hash, key.producerEpoch);
    hash = hash_value(hash, key.kind);
    hash = hash_value(hash, key.nativeKey);
    hash = hash_value(hash, key.discriminator);
    return static_cast<std::size_t>(hash);
}

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

SpatialEvidence spatial_evidence(const Node& node) noexcept {
    if (node.spatial.kind != SpatialKind::none) {
        return node.spatial;
    }
    SpatialEvidence result{};
    result.provenance = node.boundsProvenance.value_or(node.provenance);
    result.transform = node.transform;
    result.bounds = node.bounds;
    if (node.bounds.has_value()) {
        result.kind = SpatialKind::exactBounds;
    } else if (node.transform.has_value() && node.transformRuntime) {
        result.kind = SpatialKind::runtimeCenter;
    } else if (node.transform.has_value() && node.provenance == Provenance::catalog) {
        result.kind = SpatialKind::authoredPlacement;
    } else if (node.transform.has_value()) {
        result.kind = SpatialKind::helperMarker;
    }
    return result;
}

void Graph::reset(std::uint32_t generation, std::uint32_t runtimeProducerEpoch) {
    nodes_.clear();
    keys_.clear();
    root_ = {};
    generation_ = generation == 0 ? 1 : generation;
    runtimeProducerEpoch_ = runtimeProducerEpoch == 0 ? 1 : runtimeProducerEpoch;
    rejectedDuplicateKeys_ = 0;
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
    const NodeKey parentKey = key(parent);
    if (!node.key) {
        node.producerEpoch = node.producerEpoch != 0
                                 ? node.producerEpoch
                                 : (runtime_scoped(node.producer) ? runtimeProducerEpoch_ : 1U);
        node.key.producer = node.producer;
        node.key.producerEpoch = node.producerEpoch;
        node.key.kind = node.kind;
        node.key.nativeKey = node.nativeKey;
        node.key.discriminator = node.stableDiscriminator != 0
                                     ? node.stableDiscriminator
                                     : derived_discriminator(parentKey, node.name);
    }
    if (keys_.contains(node.key)) {
        ++rejectedDuplicateKeys_;
        return {};
    }
    node.spatial = spatial_evidence(node);
    if (node.nativeKey != 0) {
        append_property(node, "native_key", "Native key", node.nativeKey);
    }
    if (node.tag.has_value()) {
        append_property(node, "tag", "Tag", static_cast<std::uint64_t>(*node.tag));
    }
    if (node.classHash.has_value()) {
        append_property(
            node, "class_hash", "Class hash", static_cast<std::uint64_t>(*node.classHash));
    }
    if (node.worldId.has_value()) {
        append_property(node, "world_id", "World ID", *node.worldId);
    }
    if (node.nameHash.has_value()) {
        append_property(node, "name_hash", "Name hash", static_cast<std::uint64_t>(*node.nameHash));
    }
    if (node.runtimeEntity.has_value()) {
        append_property(node, "runtime_entity", "Runtime entity", *node.runtimeEntity);
    }
    if (node.objectSystemType.has_value()) {
        append_property(node,
                        "object_system_type",
                        "Object-system type",
                        static_cast<std::uint64_t>(*node.objectSystemType));
    }
    if (node.observationId.has_value()) {
        append_property(node, "observation_id", "Observation ID", *node.observationId);
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
    if (node.activityLogicMetadata.has_value()) {
        const ActivityLogicMetadata& logic = *node.activityLogicMetadata;
        append_hex(node.searchText, logic.scenarioTag, 8);
        append_hex(node.searchText, logic.definitionTag, 8);
        append_hex(node.searchText, logic.classPrimary, 8);
        append_hex(node.searchText, logic.classSecondary, 8);
        append_search(node.searchText, normalize(logic.roleName));
        append_search(node.searchText, normalize(logic.label));
        append_search(node.searchText, normalize(logic.localizedText));
        for (const ActivityLogicRelationship& relationship : logic.relationships) {
            append_hex(node.searchText, relationship.definitionTag, 8);
            append_hex(node.searchText, relationship.nameHash, 8);
        }
    }
    for (const Property& property : node.properties) {
        if (!property.searchable) {
            continue;
        }
        append_search(node.searchText, normalize(property.label));
        append_search(node.searchText, normalize(property.key));
        std::visit(
            [&node](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, std::string>) {
                    append_search(node.searchText, normalize(value));
                } else if constexpr (std::is_integral_v<Value>) {
                    append_decimal(node.searchText, static_cast<std::uint64_t>(value));
                }
            },
            property.value);
    }

    const std::size_t index = nodes_.size();
    nodes_.push_back(std::move(node));
    keys_.emplace(nodes_.back().key, index);
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

const Node* Graph::node(NodeKey key) const noexcept {
    const auto found = keys_.find(key);
    return found == keys_.end() ? nullptr : &nodes_[found->second];
}

NodeId Graph::resolve(NodeKey key) const noexcept {
    const Node* found = node(key);
    return found == nullptr ? NodeId{} : found->id;
}

NodeKey Graph::key(NodeId id) const noexcept {
    const Node* found = node(id);
    return found == nullptr ? NodeKey{} : found->key;
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

std::size_t Graph::rejected_duplicate_keys() const noexcept {
    return rejectedDuplicateKeys_;
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
        while (cursor < text.size()
               && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text.size()) {
            break;
        }

        QueryTerm term;
        const std::size_t tokenStart = cursor;
        while (cursor < text.size() && text[cursor] != ':'
               && std::isspace(static_cast<unsigned char>(text[cursor])) == 0) {
            ++cursor;
        }
        if (cursor < text.size() && text[cursor] == ':') {
            const std::string fieldName = normalize(text.substr(tokenStart, cursor - tokenStart));
            const std::optional<QueryTerm::Field> field = field_for(fieldName);
            if (!field.has_value()) {
                result.error = "Unknown query field: " + fieldName;
                return result;
            }
            term.field = *field;
            ++cursor;
        } else {
            cursor = tokenStart;
        }

        if (cursor < text.size() && text[cursor] == '"') {
            ++cursor;
            const std::size_t valueStart = cursor;
            while (cursor < text.size() && text[cursor] != '"') {
                ++cursor;
            }
            if (cursor >= text.size()) {
                result.error = "Unterminated quoted query value.";
                return result;
            }
            term.value = normalize(text.substr(valueStart, cursor - valueStart));
            ++cursor;
        } else {
            const std::size_t valueStart = cursor;
            while (cursor < text.size()
                   && std::isspace(static_cast<unsigned char>(text[cursor])) == 0) {
                ++cursor;
            }
            term.value = normalize(text.substr(valueStart, cursor - valueStart));
        }
        if (term.value.empty()) {
            result.error = "Query field values cannot be empty.";
            return result;
        }
        result.terms.push_back(std::move(term));
    }
    return result;
}

bool matches(const Node& node, const Query& query) noexcept {
    return query.valid() && std::ranges::all_of(query.terms, [&node](const QueryTerm& term) {
               return field_matches(node, term);
           });
}

const char* kind_name(NodeKind kind) noexcept {
    return descriptor(kind).displayName.data();
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
    return descriptor(producer).stableName.data();
}

const char* provenance_name(Provenance provenance) noexcept {
    switch (provenance) {
    case Provenance::derived:
        return "derived";
    case Provenance::catalog:
        return "catalog";
    case Provenance::runtime:
        return "runtime";
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
