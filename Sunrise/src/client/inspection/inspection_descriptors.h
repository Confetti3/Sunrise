#pragma once

#include <span>
#include <string_view>

#include "world_inspection_model.h"

namespace sunrise::client::inspection {

enum class NodeCategory : std::uint8_t {
    context,
    authored,
    entity,
    spawn,
    logic,
    geometry,
    trigger,
    audio,
    physics,
    unresolved,
};

struct NodeKindDescriptor final {
    NodeKind kind{NodeKind::unresolved};
    std::string_view stableName;
    std::string_view displayName;
    NodeCategory category{NodeCategory::unresolved};
    bool searchable{true};
};

struct ProducerDescriptor final {
    Producer producer{Producer::graph};
    std::string_view stableName;
    std::string_view displayName;
    bool optional{};
};

[[nodiscard]] std::span<const NodeKindDescriptor> node_kind_descriptors() noexcept;
[[nodiscard]] std::span<const ProducerDescriptor> producer_descriptors() noexcept;
[[nodiscard]] const NodeKindDescriptor& descriptor(NodeKind kind) noexcept;
[[nodiscard]] const ProducerDescriptor& descriptor(Producer producer) noexcept;

} // namespace sunrise::client::inspection
