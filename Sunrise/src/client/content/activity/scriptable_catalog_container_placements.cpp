#include "scriptable_catalog_container_placements.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/tables/authored_placement_reader.h"
#include "../../../middleware/content/packages/tables/component_container_reader.h"
#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../state/build_data/runtime.h"
#include "../spawn_sets/spawn_set_catalog_builder.h"
#include "scriptable_catalog_inline_names.h"

namespace sunrise::client::content::activity::scriptables::internal {

/** Fixed-capacity per-tag store of decoded container placements, shared across scenarios. */
struct ContainerPlacementCache::Impl final {
    static constexpr std::size_t kEntryCapacity = 16'384;
    static constexpr std::size_t kPlacementCapacity = 200'000;
    static constexpr std::size_t kConfigCapacity = 500'000;
    static constexpr std::size_t kComponentCapacity = 600'000;
    static constexpr std::size_t kInlineNameCapacity = 1'048'576;
    static constexpr std::size_t kInlineByteCapacity = 128ULL * 1024ULL * 1024ULL;

    struct Evidence final {
        std::vector<state::build_data::scriptables::InlineNameCandidate> rows{};
        std::vector<std::byte> bytes{};
    };

    struct Value final {
        std::vector<state::build_data::scriptables::ContainerPlacement> placements{};
        std::vector<state::build_data::scriptables::ContainerPlacementConfig> configs{};
        std::vector<state::build_data::scriptables::ContainerPlacementComponent> components{};
        Evidence evidence{};
    };

    Impl() {
        entries.reserve(kEntryCapacity);
    }

    std::unordered_map<std::uint32_t, std::shared_ptr<const Value>> entries{};
    std::size_t placementRows{};
    std::size_t configRows{};
    std::size_t componentRows{};
    std::size_t inlineNameRows{};
    std::size_t inlineBytes{};
};

/** Gives this translation unit bounded access to the cache's private storage. */
struct ContainerPlacementCacheAccess final {
    using Evidence = ContainerPlacementCache::Impl::Evidence;
    using Value = ContainerPlacementCache::Impl::Value;

    [[nodiscard]] static std::shared_ptr<const Value> find(const ContainerPlacementCache& cache,
                                                           std::uint32_t tag) noexcept {
        if (cache.impl_ == nullptr) {
            return {};
        }
        const auto found = cache.impl_->entries.find(tag);
        return found == cache.impl_->entries.end() ? std::shared_ptr<const Value>{} : found->second;
    }

    /** Stores one tag's decoded placements, dropping the insert when a capacity is reached. */
    static void remember(ContainerPlacementCache& cache, std::uint32_t tag, Value value) noexcept {
        if (cache.impl_ == nullptr || cache.impl_->entries.contains(tag)) {
            return;
        }
        ContainerPlacementCache::Impl& impl = *cache.impl_;
        if (impl.entries.size() >= ContainerPlacementCache::Impl::kEntryCapacity
            || impl.placementRows > ContainerPlacementCache::Impl::kPlacementCapacity
            || value.placements.size()
                   > ContainerPlacementCache::Impl::kPlacementCapacity - impl.placementRows
            || impl.configRows > ContainerPlacementCache::Impl::kConfigCapacity
            || value.configs.size()
                   > ContainerPlacementCache::Impl::kConfigCapacity - impl.configRows
            || impl.componentRows > ContainerPlacementCache::Impl::kComponentCapacity
            || value.components.size()
                   > ContainerPlacementCache::Impl::kComponentCapacity - impl.componentRows
            || impl.inlineNameRows > ContainerPlacementCache::Impl::kInlineNameCapacity
            || value.evidence.rows.size()
                   > ContainerPlacementCache::Impl::kInlineNameCapacity - impl.inlineNameRows
            || impl.inlineBytes > ContainerPlacementCache::Impl::kInlineByteCapacity
            || value.evidence.bytes.size()
                   > ContainerPlacementCache::Impl::kInlineByteCapacity - impl.inlineBytes) {
            return;
        }
        try {
            const std::size_t placements = value.placements.size();
            const std::size_t configs = value.configs.size();
            const std::size_t components = value.components.size();
            const std::size_t names = value.evidence.rows.size();
            const std::size_t bytes = value.evidence.bytes.size();
            auto shared = std::make_shared<Value>(std::move(value));
            const auto result = impl.entries.emplace(tag, std::move(shared));
            if (result.second) {
                impl.placementRows += placements;
                impl.configRows += configs;
                impl.componentRows += components;
                impl.inlineNameRows += names;
                impl.inlineBytes += bytes;
            }
        } catch (...) {
            // A complete result stays valid when the optional pass cache cannot grow.
        }
    }
};

namespace {

namespace catalog = state::build_data::scriptables;
namespace package_reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

/** Hard limits bound one scenario's process-only package graph. */
constexpr std::size_t kListCapacity = 8'192;
constexpr std::size_t kOwnerCapacity = 32'768;
constexpr std::size_t kPlacementCapacity = 262'144;
constexpr std::size_t kConfigCapacity = 1'048'576;
constexpr std::size_t kComponentCapacity = 1'048'576;

/** Inputs and outputs of one container placement pass. */
struct BuildContext final {
    const package_reader::Source* source{};
    package_reader::Scratch* scratch{};
    catalog::Snapshot* output{};
    ContainerPlacementCache* cache{};
    ContainerPlacementCancelCheck cancel{};
    std::string_view stem{};
    std::unordered_map<std::uint32_t, std::uint32_t> listRows{};
    std::vector<std::byte> containerBytes{};
    std::vector<std::byte> listBytes{};
    std::vector<std::byte> classBytes{};
    std::vector<std::byte> configBytes{};
    std::vector<std::byte> resourceBytes{};
    bool failed{};
};

[[nodiscard]] bool cancelled(const BuildContext& context) noexcept {
    return context.cancel != nullptr && context.cancel();
}

/** Clears padding as well as members before a raw row can reach the serialized snapshot. */
template <typename Value> void zero_row_storage(Value& output) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    std::memset(&output, 0, sizeof output);
}

/** Copies every placement member onto deterministic zeroed storage. */
void copy_placement_row(const catalog::ContainerPlacement& source,
                        catalog::ContainerPlacement& output) noexcept {
    zero_row_storage(output);
    output.listRow = source.listRow;
    output.objectListTag = source.objectListTag;
    output.entryIndex = source.entryIndex;
    output.classListTag = source.classListTag;
    output.classListNameRow = source.classListNameRow;
    output.firstConfig = source.firstConfig;
    output.configCount = source.configCount;
    output.rotation = source.rotation;
    output.position = source.position;
    output.uniformScale = source.uniformScale;
    output.placementIdentifier = source.placementIdentifier;
    output.objectType = source.objectType;
    output.placementIdentifierRead = source.placementIdentifierRead;
    output.complete = source.complete;
}

/** Copies every config member onto deterministic zeroed storage. */
void copy_config_row(const catalog::ContainerPlacementConfig& source,
                     catalog::ContainerPlacementConfig& output) noexcept {
    zero_row_storage(output);
    output.placementRow = source.placementRow;
    output.configTag = source.configTag;
    output.configNameRow = source.configNameRow;
    output.firstComponent = source.firstComponent;
    output.componentCount = source.componentCount;
    output.buildOrdinal = source.buildOrdinal;
    output.secondWord = source.secondWord;
    output.thirdWord = source.thirdWord;
    output.complete = source.complete;
}

/** Reads one reached tag and keeps all valid inline-name evidence in its blob. */
[[nodiscard]] bool read_tag(BuildContext& context,
                            std::uint32_t tag,
                            std::vector<std::byte>& output,
                            std::uint32_t& classId) noexcept {
    if (!package_reader::read_tag(*context.source, *context.scratch, tag, output, classId)) {
        return false;
    }
    if (!collect_inline_name_evidence(*context.output, output)) {
        context.failed = true;
        return false;
    }
    return true;
}

/** Marks one recoverable package read or layout failure. */
void mark_unresolved(BuildContext& context) noexcept {
    ++context.output->containerPlacementDiagnostics.unresolvedReads;
    context.output->containerPlacementDiagnostics.complete = false;
}

/** Records a lossless raw edge whose target meaning remains unresolved. */
void mark_semantic_unresolved(BuildContext& context) noexcept {
    ++context.output->containerPlacementDiagnostics.semanticUnresolved;
}

/** Marks loss that can hide a placement identity or one of its applicable owners. */
void mark_identity_owner_incomplete(BuildContext& context) noexcept {
    context.output->containerPlacementDiagnostics.identityOwnerInventoryComplete = false;
    context.output->containerPlacementDiagnostics.complete = false;
}

/** Marks one unresolved identity/owner input while retaining the general read diagnostic. */
void mark_identity_owner_unresolved(BuildContext& context) noexcept {
    mark_unresolved(context);
    mark_identity_owner_incomplete(context);
}

/** Adds a bounded loss count without allowing the diagnostic to wrap. */
void add_dropped(std::uint64_t& destination, std::size_t count) noexcept {
    const std::uint64_t available = (std::numeric_limits<std::uint64_t>::max)() - destination;
    destination += (std::min)(available, static_cast<std::uint64_t>(count));
}

/** @return True when one swept index entry belongs to the selected scenario stem. */
[[nodiscard]] bool same_stem(const BuildContext& context,
                             const ContainerIndexEntry& entry) noexcept {
    return entry.stemValid && std::string_view(entry.stem.data(), entry.stemLength) == context.stem;
}

/** @return Scenario bubble rows selected by one map-global authored mask. */
[[nodiscard]] std::uint64_t scenario_bubble_mask(
    const catalog::Snapshot& output,
    const std::array<std::uint8_t, tables::kContainerBubbleMaskBytes>& mapMask) noexcept {
    std::uint64_t result = 0;
    for (const catalog::State& state : output.states) {
        if (state.bubbleRow < std::numeric_limits<std::uint64_t>::digits
            && tables::bubble_in_mask(mapMask, state.mapBubbleIndex)) {
            result |= std::uint64_t{1} << state.bubbleRow;
        }
    }
    return result;
}

/** @return True when every transform lane is safe for distance and render math. */
template <std::size_t Size>
[[nodiscard]] bool finite(const std::array<float, Size>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float lane) { return std::isfinite(lane); });
}

enum class CacheReplay : std::uint8_t {
    unavailable,
    complete,
    cancelled,
};

/** Captures one exact evidence slice with every byte offset local to that slice. */
[[nodiscard]] bool capture_evidence(const catalog::Snapshot& output,
                                    std::size_t firstInlineName,
                                    std::size_t firstInlineByte,
                                    ContainerPlacementCacheAccess::Evidence& cached) noexcept {
    if (firstInlineName > output.inlineNameCandidates.size()
        || firstInlineByte > output.inlineNameBytes.size()
        || firstInlineByte > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    try {
        cached = {};
        cached.bytes.assign(output.inlineNameBytes.begin()
                                + static_cast<std::ptrdiff_t>(firstInlineByte),
                            output.inlineNameBytes.end());
        cached.rows.reserve(output.inlineNameCandidates.size() - firstInlineName);
        for (std::size_t index = firstInlineName; index < output.inlineNameCandidates.size();
             ++index) {
            catalog::InlineNameCandidate row = output.inlineNameCandidates[index];
            if (row.firstByte < firstInlineByte) {
                return false;
            }
            const std::size_t relative = row.firstByte - firstInlineByte;
            if (relative > cached.bytes.size() || row.byteCount > cached.bytes.size() - relative) {
                return false;
            }
            row.firstByte = static_cast<std::uint32_t>(relative);
            cached.rows.push_back(row);
        }
        return true;
    } catch (...) {
        return false;
    }
}

/** Replays one exact evidence slice or leaves both destination banks unchanged. */
[[nodiscard]] CacheReplay
replay_evidence(BuildContext& context,
                const ContainerPlacementCacheAccess::Evidence& cached) noexcept {
    if (cancelled(context)) {
        return CacheReplay::cancelled;
    }
    catalog::Snapshot& output = *context.output;
    constexpr std::size_t maximum = (std::numeric_limits<std::uint32_t>::max)();
    const std::size_t firstInlineName = output.inlineNameCandidates.size();
    const std::size_t firstInlineByte = output.inlineNameBytes.size();
    if (firstInlineName > maximum || cached.rows.size() > maximum - firstInlineName
        || firstInlineByte > maximum || cached.bytes.size() > maximum - firstInlineByte) {
        return CacheReplay::unavailable;
    }
    try {
        output.inlineNameCandidates.reserve(firstInlineName + cached.rows.size());
        output.inlineNameBytes.reserve(firstInlineByte + cached.bytes.size());
    } catch (...) {
        return CacheReplay::unavailable;
    }
    output.inlineNameBytes.insert(
        output.inlineNameBytes.end(), cached.bytes.begin(), cached.bytes.end());
    for (catalog::InlineNameCandidate row : cached.rows) {
        if (cancelled(context)) {
            output.inlineNameCandidates.resize(firstInlineName);
            output.inlineNameBytes.resize(firstInlineByte);
            return CacheReplay::cancelled;
        }
        row.firstByte += static_cast<std::uint32_t>(firstInlineByte);
        output.inlineNameCandidates.push_back(row);
    }
    if (cancelled(context)) {
        output.inlineNameCandidates.resize(firstInlineName);
        output.inlineNameBytes.resize(firstInlineByte);
        return CacheReplay::cancelled;
    }
    return CacheReplay::complete;
}

/** Captures one complete list graph with every row index local to that graph. */
[[nodiscard]] bool capture_cached_graph(const catalog::Snapshot& output,
                                        std::uint32_t listRow,
                                        std::size_t firstPlacement,
                                        std::size_t firstConfig,
                                        std::size_t firstComponent,
                                        std::size_t firstInlineName,
                                        std::size_t firstInlineByte,
                                        ContainerPlacementCacheAccess::Value& cached) noexcept {
    try {
        cached = {};
        cached.placements.assign(output.containerPlacements.begin()
                                     + static_cast<std::ptrdiff_t>(firstPlacement),
                                 output.containerPlacements.end());
        cached.configs.assign(output.containerPlacementConfigs.begin()
                                  + static_cast<std::ptrdiff_t>(firstConfig),
                              output.containerPlacementConfigs.end());
        cached.components.assign(output.containerPlacementComponents.begin()
                                     + static_cast<std::ptrdiff_t>(firstComponent),
                                 output.containerPlacementComponents.end());
        for (catalog::ContainerPlacement& placement : cached.placements) {
            if (placement.listRow != listRow || placement.firstConfig < firstConfig
                || placement.firstConfig - firstConfig > cached.configs.size()) {
                return false;
            }
            placement.listRow = 0;
            placement.firstConfig -= static_cast<std::uint32_t>(firstConfig);
        }
        for (catalog::ContainerPlacementConfig& config : cached.configs) {
            if (config.placementRow < firstPlacement
                || config.placementRow - firstPlacement >= cached.placements.size()
                || config.firstComponent < firstComponent
                || config.firstComponent - firstComponent > cached.components.size()) {
                return false;
            }
            config.placementRow -= static_cast<std::uint32_t>(firstPlacement);
            config.firstComponent -= static_cast<std::uint32_t>(firstComponent);
        }
        for (catalog::ContainerPlacementComponent& component : cached.components) {
            if (component.configRow < firstConfig
                || component.configRow - firstConfig >= cached.configs.size()) {
                return false;
            }
            component.configRow -= static_cast<std::uint32_t>(firstConfig);
        }
        return capture_evidence(output, firstInlineName, firstInlineByte, cached.evidence);
    } catch (...) {
        return false;
    }
}

/** @return True when a cached complete graph fits every remaining scenario bank. */
[[nodiscard]] bool graph_fits(const catalog::Snapshot& output,
                              const ContainerPlacementCacheAccess::Value& cached) noexcept {
    return output.containerPlacements.size() <= kPlacementCapacity
           && cached.placements.size() <= kPlacementCapacity - output.containerPlacements.size()
           && output.containerPlacementConfigs.size() <= kConfigCapacity
           && cached.configs.size() <= kConfigCapacity - output.containerPlacementConfigs.size()
           && output.containerPlacementComponents.size() <= kComponentCapacity
           && cached.components.size()
                  <= kComponentCapacity - output.containerPlacementComponents.size();
}

/** Replays one complete graph or leaves every destination bank unchanged. */
[[nodiscard]] CacheReplay
replay_cached_graph(BuildContext& context,
                    std::uint32_t listRow,
                    const ContainerPlacementCacheAccess::Value& cached) noexcept {
    if (cancelled(context)) {
        return CacheReplay::cancelled;
    }
    catalog::Snapshot& output = *context.output;
    if (!graph_fits(output, cached)) {
        return CacheReplay::unavailable;
    }
    constexpr std::size_t maximum = (std::numeric_limits<std::uint32_t>::max)();
    const std::size_t firstPlacement = output.containerPlacements.size();
    const std::size_t firstConfig = output.containerPlacementConfigs.size();
    const std::size_t firstComponent = output.containerPlacementComponents.size();
    const std::size_t firstInlineName = output.inlineNameCandidates.size();
    const std::size_t firstInlineByte = output.inlineNameBytes.size();
    if (firstInlineName > maximum || cached.evidence.rows.size() > maximum - firstInlineName
        || firstInlineByte > maximum || cached.evidence.bytes.size() > maximum - firstInlineByte) {
        return CacheReplay::unavailable;
    }
    try {
        output.containerPlacements.reserve(firstPlacement + cached.placements.size());
        output.containerPlacementConfigs.reserve(firstConfig + cached.configs.size());
        output.containerPlacementComponents.reserve(firstComponent + cached.components.size());
        output.inlineNameCandidates.reserve(firstInlineName + cached.evidence.rows.size());
        output.inlineNameBytes.reserve(firstInlineByte + cached.evidence.bytes.size());
    } catch (...) {
        return CacheReplay::unavailable;
    }
    const auto rollback = [&output,
                           firstPlacement,
                           firstConfig,
                           firstComponent,
                           firstInlineName,
                           firstInlineByte]() noexcept {
        output.containerPlacements.resize(firstPlacement);
        output.containerPlacementConfigs.resize(firstConfig);
        output.containerPlacementComponents.resize(firstComponent);
        output.inlineNameCandidates.resize(firstInlineName);
        output.inlineNameBytes.resize(firstInlineByte);
    };
    output.inlineNameBytes.insert(
        output.inlineNameBytes.end(), cached.evidence.bytes.begin(), cached.evidence.bytes.end());
    for (catalog::InlineNameCandidate row : cached.evidence.rows) {
        if (cancelled(context)) {
            rollback();
            return CacheReplay::cancelled;
        }
        row.firstByte += static_cast<std::uint32_t>(firstInlineByte);
        output.inlineNameCandidates.push_back(row);
    }
    for (const catalog::ContainerPlacement& source : cached.placements) {
        if (cancelled(context)) {
            rollback();
            return CacheReplay::cancelled;
        }
        output.containerPlacements.emplace_back();
        catalog::ContainerPlacement& row = output.containerPlacements.back();
        copy_placement_row(source, row);
        row.listRow = listRow;
        row.firstConfig += static_cast<std::uint32_t>(firstConfig);
    }
    for (const catalog::ContainerPlacementConfig& source : cached.configs) {
        if (cancelled(context)) {
            rollback();
            return CacheReplay::cancelled;
        }
        output.containerPlacementConfigs.emplace_back();
        catalog::ContainerPlacementConfig& row = output.containerPlacementConfigs.back();
        copy_config_row(source, row);
        row.placementRow += static_cast<std::uint32_t>(firstPlacement);
        row.firstComponent += static_cast<std::uint32_t>(firstComponent);
    }
    for (catalog::ContainerPlacementComponent row : cached.components) {
        if (cancelled(context)) {
            rollback();
            return CacheReplay::cancelled;
        }
        row.configRow += static_cast<std::uint32_t>(firstConfig);
        output.containerPlacementComponents.push_back(row);
    }
    if (cancelled(context)) {
        rollback();
        return CacheReplay::cancelled;
    }
    return CacheReplay::complete;
}

/** Reads one tag only when its class matches the typed edge being followed. */
[[nodiscard]] bool read_exact(BuildContext& context,
                              std::uint32_t tag,
                              std::uint32_t expectedClass,
                              std::vector<std::byte>& output) noexcept {
    std::uint32_t classId = 0;
    if (cancelled(context)) {
        context.failed = true;
        return false;
    }
    if (!read_tag(context, tag, output, classId) || classId != expectedClass) {
        mark_unresolved(context);
        return false;
    }
    return true;
}

/** Appends the exact typed component rows of one placed config. */
[[nodiscard]] bool
append_components(BuildContext& context, std::uint32_t configRow, std::span<const std::byte> blob) {
    catalog::ContainerPlacementConfig& config =
        context.output->containerPlacementConfigs[configRow];
    config.firstComponent =
        static_cast<std::uint32_t>(context.output->containerPlacementComponents.size());
    tables::Array components{};
    if (!tables::placed_config_components(blob, components)) {
        mark_unresolved(context);
        return false;
    }
    const std::size_t retained =
        (std::min)(static_cast<std::size_t>(components.count),
                   kComponentCapacity - context.output->containerPlacementComponents.size());
    for (std::size_t index = 0; index < retained; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        tables::PlacedConfigComponentRow source{};
        if (!tables::placed_config_component_at(blob, components, index, source)) {
            mark_unresolved(context);
            return false;
        }
        context.output->containerPlacementComponents.push_back({configRow,
                                                                source.componentClass,
                                                                source.firstWord,
                                                                source.secondWord,
                                                                source.fourthWord,
                                                                static_cast<std::uint32_t>(index)});
    }
    config.componentCount = static_cast<std::uint32_t>(retained);
    if (retained != components.count) {
        add_dropped(context.output->containerPlacementDiagnostics.droppedComponents,
                    static_cast<std::size_t>(components.count) - retained);
        context.output->containerPlacementDiagnostics.complete = false;
        return false;
    }
    return true;
}

/** Appends one config occurrence and its exact component edges. */
[[nodiscard]] bool append_config(BuildContext& context,
                                 std::uint32_t placementRow,
                                 std::uint32_t buildOrdinal,
                                 const tables::PlacedClassBuildRow& source) {
    if (context.output->containerPlacementConfigs.size() >= kConfigCapacity) {
        ++context.output->containerPlacementDiagnostics.droppedConfigs;
        context.output->containerPlacementDiagnostics.complete = false;
        return false;
    }
    context.output->containerPlacementConfigs.emplace_back();
    catalog::ContainerPlacementConfig& destination =
        context.output->containerPlacementConfigs.back();
    zero_row_storage(destination);
    destination.placementRow = placementRow;
    destination.configTag = source.configTag;
    destination.configNameRow = catalog::kNoRow;
    destination.buildOrdinal = buildOrdinal;
    destination.secondWord = source.secondWord;
    destination.thirdWord = source.thirdWord;
    const std::uint32_t row =
        static_cast<std::uint32_t>(context.output->containerPlacementConfigs.size() - 1);
    bool complete =
        read_exact(context, source.configTag, tables::kPlacedConfigClass, context.configBytes);
    if (complete) {
        complete = append_components(context, row, context.configBytes);
    }
    context.output->containerPlacementConfigs[row].complete = complete && !context.failed;
    return complete;
}

/** Appends the ordered config graph of one exact placed class definition. */
[[nodiscard]] bool
append_class_graph(BuildContext& context, std::uint32_t placementRow, std::uint32_t classTag) {
    catalog::ContainerPlacement& placement = context.output->containerPlacements[placementRow];
    if (!read_exact(context, classTag, tables::kPlacedClassDefinitionClass, context.classBytes)) {
        return false;
    }
    tables::PlacedClassDefinition source{};
    if (!tables::placed_class_definition(context.classBytes, source)) {
        mark_unresolved(context);
        return false;
    }
    placement.objectType = source.objectType;
    placement.firstConfig =
        static_cast<std::uint32_t>(context.output->containerPlacementConfigs.size());
    bool complete = true;
    for (std::size_t index = 0; index < source.builds.count; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        tables::PlacedClassBuildRow build{};
        if (!tables::placed_class_build_at(context.classBytes, source.builds, index, build)) {
            mark_unresolved(context);
            complete = false;
            break;
        }
        if (!append_config(context, placementRow, static_cast<std::uint32_t>(index), build)) {
            complete = false;
            if (context.output->containerPlacementConfigs.size() >= kConfigCapacity) {
                add_dropped(context.output->containerPlacementDiagnostics.droppedConfigs,
                            static_cast<std::size_t>(source.builds.count) - index - 1);
                break;
            }
        }
    }
    placement.configCount = static_cast<std::uint32_t>(
        context.output->containerPlacementConfigs.size() - placement.firstConfig);
    return complete && placement.configCount == source.builds.count;
}

/** Appends every exact transform from one unique object list. */
[[nodiscard]] bool
append_placements(BuildContext& context, std::uint32_t listRow, std::uint32_t objectListTag) {
    tables::Array placements{};
    if (!tables::authored_placements(context.listBytes, placements)) {
        mark_identity_owner_unresolved(context);
        return false;
    }
    const std::size_t retained =
        (std::min)(static_cast<std::size_t>(placements.count),
                   kPlacementCapacity - context.output->containerPlacements.size());
    bool complete = retained == placements.count;
    if (!complete) {
        add_dropped(context.output->containerPlacementDiagnostics.droppedPlacements,
                    static_cast<std::size_t>(placements.count) - retained);
        mark_identity_owner_incomplete(context);
    }
    for (std::size_t index = 0; index < retained; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        tables::AuthoredPlacement source{};
        float uniformScale = 0.0F;
        std::uint64_t placementIdentifier = 0;
        if (!tables::authored_placement_at(context.listBytes, placements, index, source)
            || !tables::authored_placement_identifier_at(
                context.listBytes, placements, index, placementIdentifier)
            || !tables::container_placement_uniform_scale_at(
                context.listBytes, placements, index, uniformScale)
            || !finite(source.rotation) || !finite(source.position)
            || !std::isfinite(uniformScale)) {
            ++context.output->containerPlacementDiagnostics.droppedPlacements;
            mark_identity_owner_unresolved(context);
            complete = false;
            continue;
        }
        context.output->containerPlacements.emplace_back();
        catalog::ContainerPlacement& destination = context.output->containerPlacements.back();
        zero_row_storage(destination);
        destination.listRow = listRow;
        destination.objectListTag = objectListTag;
        destination.entryIndex = static_cast<std::uint32_t>(index);
        destination.classListTag = source.classListTag;
        destination.classListNameRow = catalog::kNoRow;
        destination.rotation = source.rotation;
        destination.position = source.position;
        destination.uniformScale = uniformScale;
        destination.placementIdentifier = placementIdentifier;
        destination.placementIdentifierRead = true;
        const std::uint32_t row =
            static_cast<std::uint32_t>(context.output->containerPlacements.size() - 1);
        const bool rowComplete = append_class_graph(context, row, source.classListTag);
        context.output->containerPlacements[row].complete = rowComplete;
        complete = complete && rowComplete;
    }
    return complete;
}

/** Reads the list-level resource edge and every placement below one unique list. */
void materialize_list(BuildContext& context, std::uint32_t listRow) {
    catalog::ContainerPlacementList& list = context.output->containerPlacementLists[listRow];
    // TODO: no consumer for a list-level resource edge until the component member shape is read.
    // `component_resource` reads +216, a component field. An object list keeps every list-level
    // field below its data offset of 48, so +216 lands inside placement entry 1's rotation.
    if (context.cache != nullptr) {
        const auto cached = ContainerPlacementCacheAccess::find(*context.cache, list.objectListTag);
        if (cached != nullptr) {
            const CacheReplay replay = replay_cached_graph(context, listRow, *cached);
            if (replay == CacheReplay::complete) {
                list.complete = true;
                return;
            }
            if (replay == CacheReplay::cancelled) {
                context.failed = true;
                list.complete = false;
                return;
            }
        }
    }
    const std::size_t firstPlacement = context.output->containerPlacements.size();
    const std::size_t firstConfig = context.output->containerPlacementConfigs.size();
    const std::size_t firstComponent = context.output->containerPlacementComponents.size();
    const std::size_t firstInlineName = context.output->inlineNameCandidates.size();
    const std::size_t firstInlineByte = context.output->inlineNameBytes.size();
    list.complete = append_placements(context, listRow, list.objectListTag);
    if (list.complete && !context.failed && !cancelled(context) && context.cache != nullptr) {
        ContainerPlacementCacheAccess::Value cached{};
        if (capture_cached_graph(*context.output,
                                 listRow,
                                 firstPlacement,
                                 firstConfig,
                                 firstComponent,
                                 firstInlineName,
                                 firstInlineByte,
                                 cached)) {
            ContainerPlacementCacheAccess::remember(
                *context.cache, list.objectListTag, std::move(cached));
        }
    }
}

/** @return Existing list row, or a new row after its graph is materialized. */
[[nodiscard]] std::uint32_t ensure_list(BuildContext& context, std::uint32_t tag) {
    const auto found = context.listRows.find(tag);
    if (found != context.listRows.end()) {
        return found->second;
    }
    if (context.output->containerPlacementLists.size() >= kListCapacity) {
        ++context.output->containerPlacementDiagnostics.droppedLists;
        mark_identity_owner_incomplete(context);
        return catalog::kNoRow;
    }
    const std::uint32_t row =
        static_cast<std::uint32_t>(context.output->containerPlacementLists.size());
    context.output->containerPlacementLists.emplace_back();
    catalog::ContainerPlacementList& list = context.output->containerPlacementLists.back();
    zero_row_storage(list);
    list.objectListTag = tag;
    list.objectListNameRow = catalog::kNoRow;
    list.resourceNameRow = catalog::kNoRow;
    context.listRows.emplace(tag, row);
    materialize_list(context, row);
    return row;
}

/** Appends one exact container member edge with its independent authored mask. */
void append_owner(BuildContext& context,
                  std::uint32_t listRow,
                  std::uint32_t containerTag,
                  std::uint32_t memberIndex,
                  std::uint64_t scenarioMask,
                  const std::array<std::uint8_t, tables::kContainerBubbleMaskBytes>& mapMask) {
    if (context.output->containerPlacementOwners.size() >= kOwnerCapacity) {
        ++context.output->containerPlacementDiagnostics.droppedOwners;
        mark_identity_owner_incomplete(context);
        return;
    }
    context.output->containerPlacementOwners.emplace_back();
    catalog::ContainerPlacementOwner& owner = context.output->containerPlacementOwners.back();
    zero_row_storage(owner);
    owner.listRow = listRow;
    owner.containerTag = containerTag;
    owner.memberIndex = memberIndex;
    owner.containerNameRow = catalog::kNoRow;
    owner.scenarioBubbleMask = scenarioMask;
    owner.mapBubbleMask = mapMask;
    owner.context = catalog::SpatialContextJoin::packageStemBubble;
}

/** Reads one selected-stem container and follows every exact object-list member. */
[[nodiscard]] bool collect_container_impl(BuildContext& context, std::uint32_t containerTag) {
    if (cancelled(context)) {
        context.failed = true;
        return false;
    }
    if (!read_exact(context, containerTag, tables::kContainerClass, context.containerBytes)) {
        mark_identity_owner_incomplete(context);
        return !context.failed;
    }
    std::array<std::uint8_t, tables::kContainerBubbleMaskBytes> mapMask{};
    tables::Array members{};
    if (!tables::container_bubble_mask(context.containerBytes, mapMask)
        || !tables::container_members(context.containerBytes, members)
        || members.elementClass != tables::kContainerMemberClass) {
        mark_identity_owner_unresolved(context);
        return true;
    }
    const std::uint64_t scenarioMask = scenario_bubble_mask(*context.output, mapMask);
    if (scenarioMask == 0) {
        return true;
    }
    for (std::size_t index = 0; index < members.count; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        std::uint32_t memberTag = 0;
        std::uint32_t memberClass = 0;
        if (!tables::container_member_at(context.containerBytes, members, index, memberTag)) {
            mark_identity_owner_unresolved(context);
            break;
        }
        if (!read_tag(context, memberTag, context.listBytes, memberClass)) {
            mark_identity_owner_unresolved(context);
            continue;
        }
        if (memberClass != tables::kAuthoredPlacementListClass) {
            continue;
        }
        const std::uint32_t listRow = ensure_list(context, memberTag);
        if (listRow != catalog::kNoRow) {
            append_owner(context,
                         listRow,
                         containerTag,
                         static_cast<std::uint32_t>(index),
                         scenarioMask,
                         mapMask);
        }
    }
    return !context.failed;
}

/** Converts exceptions at the container boundary into one failed build. */
[[nodiscard]] bool collect_container(BuildContext& context, std::uint32_t containerTag) noexcept {
    try {
        return collect_container_impl(context, containerTag);
    } catch (...) {
        mark_identity_owner_incomplete(context);
        context.failed = true;
        return false;
    }
}

/** Implements one container-placement build with optional pass-local graph reuse. */
[[nodiscard]] bool append_container_placements_impl(const package_reader::Source& source,
                                                    package_reader::Scratch& scratch,
                                                    ContainerPlacementCache* cache,
                                                    const ContainerIndex& containers,
                                                    std::string_view scenarioName,
                                                    catalog::Snapshot& output,
                                                    ContainerPlacementCancelCheck cancel) noexcept {
    output.containerPlacementDiagnostics = {};
    state::build_data::scenarios::Definition scenario{};
    if (!state::build_data::find_scenario_layout(scenarioName, scenario)) {
        output.containerPlacementDiagnostics.unresolvedReads = 1;
        return true;
    }
    if (scenario.spawnStemLength == 0) {
        output.containerPlacementDiagnostics.contextNotApplicable = true;
        output.containerPlacementDiagnostics.identityOwnerInventoryComplete = true;
        output.containerPlacementDiagnostics.complete = true;
        return true;
    }
    try {
        BuildContext context{};
        context.source = &source;
        context.scratch = &scratch;
        context.output = &output;
        context.cache = cache;
        context.cancel = cancel;
        context.stem = std::string_view(scenario.spawnStem.data(), scenario.spawnStemLength);
        auto& diagnostics = output.containerPlacementDiagnostics;
        diagnostics.contextResolved = true;
        diagnostics.identityOwnerInventoryComplete = true;
        diagnostics.complete = true;
        output.containerPlacementLists.reserve(512);
        output.containerPlacementOwners.reserve(1'024);
        output.containerPlacements.reserve(8'192);
        output.containerPlacementConfigs.reserve(32'768);
        output.containerPlacementComponents.reserve(32'768);
        context.listRows.reserve(1'024);
        // The index is swept once per pass. A refusal there is one unresolved read here, not one
        // per package, because no scenario can see what the sweep never reported.
        if (!containers.complete || !containers.stemsComplete) {
            mark_identity_owner_unresolved(context);
        }
        for (const ContainerIndexEntry& entry : containers.entries) {
            if (context.failed || cancelled(context)) {
                break;
            }
            if (same_stem(context, entry) && !collect_container(context, entry.tag)) {
                break;
            }
        }
        const bool finished = !context.failed && !cancelled(context);
        if (!finished) {
            mark_identity_owner_incomplete(context);
        }
        return finished;
    } catch (...) {
        output.containerPlacementDiagnostics.identityOwnerInventoryComplete = false;
        output.containerPlacementDiagnostics.complete = false;
        return false;
    }
}

} // namespace

/** Allocates optional pass storage without making extraction depend on it. */
ContainerPlacementCache::ContainerPlacementCache() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
        impl_.reset();
    }
}

ContainerPlacementCache::~ContainerPlacementCache() = default;

/** Appends the selected-stem container placement graph without assigning ClientRef identity. */
bool append_container_placements(const package_reader::Source& source,
                                 package_reader::Scratch& scratch,
                                 const ContainerIndex& containers,
                                 std::string_view scenarioName,
                                 catalog::Snapshot& output,
                                 ContainerPlacementCancelCheck cancel) noexcept {
    return append_container_placements_impl(
        source, scratch, nullptr, containers, scenarioName, output, cancel);
}

/** Appends container placements with exact pass-local object-list graph reuse. */
bool append_container_placements(const package_reader::Source& source,
                                 package_reader::Scratch& scratch,
                                 ContainerPlacementCache& cache,
                                 const ContainerIndex& containers,
                                 std::string_view scenarioName,
                                 catalog::Snapshot& output,
                                 ContainerPlacementCancelCheck cancel) noexcept {
    return append_container_placements_impl(
        source, scratch, &cache, containers, scenarioName, output, cancel);
}

} // namespace sunrise::client::content::activity::scriptables::internal
