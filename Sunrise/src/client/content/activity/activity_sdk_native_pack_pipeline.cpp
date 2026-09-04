#include "activity_sdk_native_pack_pipeline.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/tables/activity_display_name_reader.h"
#include "../../../state/activity_sdk/runtime.h"
#include "activity_sdk_activity_enrichment_inventory.h"
#include "activity_sdk_actor_rsat_inventory.h"
#include "activity_sdk_authored_scene_inventory.h"
#include "activity_sdk_behavior_inventory.h"
#include "activity_sdk_dialogue_group_index.h"
#include "activity_sdk_lua_artifacts.h"
#include "activity_sdk_pack_composer.h"
#include "activity_sdk_policy_input_adapter.h"
#include "activity_sdk_policy_inventory.h"
#include "activity_sdk_squad_inventory.h"
#include "activity_sdk_topology_enrichment.h"

namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline {
namespace {

namespace reader = middleware::content::packages::reader;
namespace format = state::activity_sdk::format;
namespace activity_enrichment = activity_enrichment_inventory;
namespace actor_rsat = actor_rsat_inventory;
namespace authored_scene = authored_scene_inventory;
namespace behaviors = behavior_inventory;
namespace dialogue_groups = dialogue_group_index;
namespace lua = lua_artifacts;
namespace composer = pack_composer;
namespace policy_adapter = policy_input_adapter;
namespace policy = policy_inventory;
namespace squads = squad_inventory;
namespace topology_enrichment = sdk_generation::topology_enrichment;
namespace display = middleware::content::packages::tables::activity_display_names;

static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));

/** One cache-owning package reader shared by squad and authored-scene extraction. */
struct PackageContext final {
    const reader::Source* source{};
    std::unique_ptr<reader::Scratch> scratch{};
    CancelProbe cancel{};
    void* cancelContext{};

    ~PackageContext() noexcept {
        if (scratch != nullptr) {
            reader::close_files(*scratch);
        }
    }
};

[[nodiscard]] bool cancelled(CancelProbe probe, void* context) noexcept {
    return probe != nullptr && probe(context);
}

[[nodiscard]] bool collect_tag(void* opaque, std::uint32_t tag) noexcept {
    if (opaque == nullptr || tag == 0 || tag == format::kAbsentIndex) {
        return false;
    }
    try {
        static_cast<std::vector<std::uint32_t>*>(opaque)->push_back(tag);
        return true;
    } catch (...) {
        return false;
    }
}

void report(ProgressProbe probe, void* context, Phase phase) noexcept {
    if (probe != nullptr) {
        probe(context, phase);
    }
}

/** Creates the empty lua child the publication commit renames when declarations are off. */
[[nodiscard]] bool ensure_lua_directory(const wchar_t* sdkDirectory) noexcept {
    std::wstring path;
    try {
        path.assign(sdkDirectory);
        path.append(L"\\lua");
    } catch (...) {
        return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
        return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** Adapts the checked package reader to the squad and authored-scene boundaries. */
[[nodiscard]] bool read_tag(void* opaque,
                            std::uint32_t tag,
                            std::vector<std::byte>& bytes,
                            std::uint32_t& classId) noexcept {
    bytes.clear();
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PackageContext*>(opaque);
    return context.source != nullptr && context.scratch != nullptr
           && !cancelled(context.cancel, context.cancelContext)
           && reader::read_tag(*context.source, *context.scratch, tag, bytes, classId);
}

[[nodiscard]] bool read_localized_tag(void* opaque,
                                      std::uint32_t tag,
                                      std::uint32_t expectedClass,
                                      std::vector<std::byte>& bytes) noexcept {
    std::uint32_t classId = 0;
    return read_tag(opaque, tag, bytes, classId) && classId == expectedClass;
}

template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> bytes, std::size_t offset, Value& output) noexcept {
    output = {};
    if (offset > bytes.size() || sizeof output > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&output, bytes.data() + offset, sizeof output);
    return true;
}

[[nodiscard]] bool
add_relative(std::size_t member, std::int64_t relative, std::size_t& target) noexcept {
    if (relative >= 0) {
        const auto distance = static_cast<std::uint64_t>(relative);
        if (distance > (std::numeric_limits<std::size_t>::max)() - member) {
            return false;
        }
        target = member + static_cast<std::size_t>(distance);
        return true;
    }
    const auto distance = static_cast<std::uint64_t>(-(relative + 1)) + 1U;
    if (distance > member) {
        return false;
    }
    target = member - static_cast<std::size_t>(distance);
    return true;
}

/** Reads one array field's data offset and count, checking its declared class and stride. */
[[nodiscard]] bool read_array(std::span<const std::byte> bytes,
                              std::size_t field,
                              std::size_t stride,
                              std::uint32_t expectedClass,
                              std::size_t& data,
                              std::size_t& count) noexcept {
    std::uint64_t rawCount = 0;
    std::int64_t relative = 0;
    std::size_t header = 0;
    std::uint64_t repeated = 0;
    std::uint32_t classId = 0;
    std::uint32_t padding = 0;
    if (!read_value(bytes, field, rawCount) || rawCount > format::kAbsentIndex
        || !read_value(bytes, field + 8U, relative) || !add_relative(field + 8U, relative, header)
        || !read_value(bytes, header, repeated) || repeated != rawCount
        || !read_value(bytes, header + 8U, classId) || classId != expectedClass
        || !read_value(bytes, header + 12U, padding) || padding != 0
        || rawCount > (std::numeric_limits<std::size_t>::max)() / stride) {
        return false;
    }
    data = header + 16U;
    count = static_cast<std::size_t>(rawCount);
    return data <= bytes.size() && count * stride <= bytes.size() - data;
}

[[nodiscard]] bool copy_text(std::string_view value, authored_scene::Text& output) noexcept {
    output = {};
    if (value.empty() || value.size() >= output.value.size()
        || value.size() > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    std::copy(value.begin(), value.end(), output.value.begin());
    output.length = static_cast<std::uint16_t>(value.size());
    return true;
}

struct AuthoredTextCandidate final {
    display::Reference reference{};
    std::uint32_t slotIndex{};
    std::uint32_t cueIndex{format::kAbsentIndex};
    std::uint32_t definitionHash{};
};

/** The two localized fields are one directive element, not two selectable directives. */
struct AuthoredDirectiveCandidate final {
    display::Reference title{};
    display::Reference description{};
    std::uint32_t slotIndex{};
    std::uint32_t nameHash{};
    std::int32_t elementIndex{-1};
    std::uint32_t elementCount{};
};

/** Extracts localized dialogue aliases and safe authored directive elements. */
[[nodiscard]] bool attach_authored_text(const topology_inventory::Snapshot& topology,
                                        const squads::Facts& facts,
                                        PackageContext& packageContext,
                                        authored_scene::Snapshot& output) {
    struct CachedTag final {
        std::vector<std::byte> bytes{};
        std::uint32_t classId{};
    };
    std::unordered_map<std::uint32_t, CachedTag> cache{};
    auto package = [&](std::uint32_t tag, const CachedTag*& result) -> bool {
        const auto found = cache.find(tag);
        if (found != cache.end()) {
            result = &found->second;
            return true;
        }
        CachedTag row{};
        if (!read_tag(&packageContext, tag, row.bytes, row.classId)) {
            return false;
        }
        result = &cache.emplace(tag, std::move(row)).first->second;
        return true;
    };
    try {
        std::vector<AuthoredTextCandidate> dialogueCandidates{};
        std::vector<AuthoredDirectiveCandidate> directiveCandidates{};
        for (const squads::DescriptorFact& descriptor : facts.descriptors) {
            if (descriptor.slotIndex >= topology.slots.size()) {
                continue;
            }
            const std::uint32_t slotType = topology.slots[descriptor.slotIndex].slotType;
            if (slotType != format::kDialogueSlotType && slotType != 68U) {
                continue;
            }
            const CachedTag* config = nullptr;
            if (!package(descriptor.configTag, config) || config == nullptr) {
                continue;
            }
            const std::size_t field = static_cast<std::size_t>(descriptor.descriptorOffset)
                                      + (slotType == format::kDialogueSlotType ? 0x58U : 0x5CU);
            std::uint32_t resourceTag = 0;
            const CachedTag* resource = nullptr;
            if (!read_value(std::span(config->bytes), field, resourceTag)
                || !package(resourceTag, resource) || resource == nullptr) {
                continue;
            }
            const auto bytes = std::span(resource->bytes);
            if (slotType == format::kDialogueSlotType) {
                if (resource->classId != format::kDialogueAuthoredListClass) {
                    continue;
                }
                std::size_t groups = 0;
                std::size_t groupCount = 0;
                if (!read_array(
                        bytes, 0x18U, 16U, format::kDialogueGroupArrayClass, groups, groupCount)) {
                    continue;
                }
                std::size_t definitions = 0;
                std::size_t definitionCount = 0;
                if (!read_array(bytes,
                                8U,
                                8U,
                                format::kDialogueDefinitionArrayClass,
                                definitions,
                                definitionCount)) {
                    continue;
                }
                std::vector<dialogue_groups::Span> groupIndex{};
                if (!dialogue_groups::build(bytes, groups, groupCount, groupIndex)) {
                    continue;
                }
                for (std::size_t cue = 0; cue < definitionCount; ++cue) {
                    std::uint32_t definitionHash = 0;
                    if (!read_value(bytes, definitions + cue * 8U, definitionHash)
                        || definitionHash == 0 || definitionHash == 0x811C9DC5U) {
                        continue;
                    }
                    dialogue_groups::Span group{};
                    if (!dialogue_groups::find(groupIndex, definitionHash, group)) {
                        continue;
                    }
                    for (std::size_t offset = group.begin; offset + 8U <= group.end; offset += 4U) {
                        std::uint32_t containerTag = 0;
                        std::uint32_t stringHash = 0;
                        const CachedTag* container = nullptr;
                        if (!read_value(bytes, offset, containerTag)
                            || !read_value(bytes, offset + 4U, stringHash)
                            || containerTag < 0x80A12000U || containerTag >= 0xC0000000U
                            || !package(containerTag, container) || container == nullptr
                            || container->classId != display::kStringContainerClass) {
                            continue;
                        }
                        dialogueCandidates.push_back({{containerTag, stringHash},
                                                      descriptor.slotIndex,
                                                      static_cast<std::uint32_t>(cue),
                                                      definitionHash});
                    }
                }
            } else {
                if (resource->classId != 0x80804F72U) {
                    continue;
                }
                std::size_t entries = 0;
                std::size_t entryCount = 0;
                if (!read_array(bytes, 8U, 40U, 0x80804F74U, entries, entryCount)) {
                    continue;
                }
                for (std::size_t entry = 0; entry < entryCount; ++entry) {
                    const std::size_t row = entries + entry * 40U;
                    std::uint32_t nameHash = 0;
                    std::int64_t relative = 0;
                    std::size_t elements = 0;
                    std::uint64_t elementCount = 0;
                    std::uint32_t elementClass = 0;
                    if (!read_value(bytes, row, nameHash) || !read_value(bytes, row + 24U, relative)
                        || !add_relative(row + 24U, relative, elements)
                        || !read_value(bytes, elements, elementCount) || elementCount == 0
                        || elementCount > format::kAbsentIndex
                        || !read_value(bytes, elements + 8U, elementClass)
                        || elementClass != 0x80804F76U) {
                        continue;
                    }
                    const std::size_t data = elements + 16U;
                    if (data > bytes.size() || elementCount > (bytes.size() - data) / 36U) {
                        continue;
                    }
                    for (std::size_t element = 0; element < elementCount; ++element) {
                        const std::size_t elementRow = data + element * 36U;
                        AuthoredDirectiveCandidate candidate{};
                        candidate.slotIndex = descriptor.slotIndex;
                        candidate.nameHash = nameHash;
                        candidate.elementIndex = static_cast<std::int32_t>(element);
                        candidate.elementCount = static_cast<std::uint32_t>(elementCount);
                        if (!read_value(bytes, elementRow, candidate.title.containerTag)
                            || !read_value(bytes, elementRow + 4U, candidate.title.stringHash)
                            || !read_value(
                                bytes, elementRow + 8U, candidate.description.containerTag)
                            || !read_value(
                                bytes, elementRow + 12U, candidate.description.stringHash)) {
                            continue;
                        }
                        const CachedTag* title = nullptr;
                        const CachedTag* description = nullptr;
                        if (!package(candidate.title.containerTag, title) || title == nullptr
                            || title->classId != display::kStringContainerClass
                            || !package(candidate.description.containerTag, description)
                            || description == nullptr
                            || description->classId != display::kStringContainerClass) {
                            continue;
                        }
                        directiveCandidates.push_back(candidate);
                    }
                }
            }
        }
        std::vector<display::Reference> references{};
        references.reserve(dialogueCandidates.size() + directiveCandidates.size() * 2U);
        for (const AuthoredTextCandidate& row : dialogueCandidates) {
            references.push_back(row.reference);
        }
        for (const AuthoredDirectiveCandidate& row : directiveCandidates) {
            references.push_back(row.title);
            references.push_back(row.description);
        }
        if (references.empty()) {
            return true;
        }
        display::Snapshot names{};
        if (!display::resolve({&packageContext, &read_localized_tag, 0, 0}, references, names)
            || names.names.size() != references.size()) {
            return true;
        }
        for (std::size_t index = 0; index < dialogueCandidates.size(); ++index) {
            const display::Name& name = names.names[index];
            if (name.authoredEmpty || name.length == 0) {
                continue;
            }
            const std::string_view text(name.value.data(), name.length);
            char id[96]{};
            const AuthoredTextCandidate& candidate = dialogueCandidates[index];
            const int length = std::snprintf(id,
                                             sizeof id,
                                             "dialogue/%08x/%u/%08x/%08x",
                                             candidate.slotIndex,
                                             candidate.cueIndex,
                                             candidate.definitionHash,
                                             candidate.reference.stringHash);
            authored_scene::DialogueCueText row{};
            if (length <= 0 || static_cast<std::size_t>(length) >= sizeof id
                || !copy_text(std::string_view(id, static_cast<std::size_t>(length)), row.id)
                || !copy_text(text, row.text)) {
                continue;
            }
            row.slotIndex = candidate.slotIndex;
            row.cueIndex = candidate.cueIndex;
            row.definitionHash = candidate.definitionHash;
            row.containerTag = candidate.reference.containerTag;
            row.stringHash = candidate.reference.stringHash;
            output.dialogueCueTexts.push_back(row);
        }
        std::size_t resolved = dialogueCandidates.size();
        for (const AuthoredDirectiveCandidate& candidate : directiveCandidates) {
            const display::Name& title = names.names[resolved++];
            const display::Name& description = names.names[resolved++];
            if (title.authoredEmpty || title.length == 0 || description.authoredEmpty
                || description.length == 0) {
                continue;
            }
            char id[96]{};
            const int length = std::snprintf(id,
                                             sizeof id,
                                             "directive/%08x/%08x/%d",
                                             candidate.slotIndex,
                                             candidate.nameHash,
                                             candidate.elementIndex);
            authored_scene::DirectiveElement row{};
            if (length <= 0 || static_cast<std::size_t>(length) >= sizeof id
                || !copy_text(std::string_view(id, static_cast<std::size_t>(length)), row.id)
                || !copy_text(std::string_view(title.value.data(), title.length), row.title)
                || !copy_text(std::string_view(description.value.data(), description.length),
                              row.description)) {
                continue;
            }
            row.slotIndex = candidate.slotIndex;
            row.nameHash = candidate.nameHash;
            row.elementIndex = candidate.elementIndex;
            row.elementCount = candidate.elementCount;
            row.titleContainerTag = candidate.title.containerTag;
            row.titleStringHash = candidate.title.stringHash;
            row.descriptionContainerTag = candidate.description.containerTag;
            row.descriptionStringHash = candidate.description.stringHash;
            output.directiveElements.push_back(row);
        }
        auto dialogueLess = [](const auto& left, const auto& right) {
            return std::tie(left.slotIndex, left.cueIndex, left.definitionHash, left.stringHash)
                   < std::tie(
                       right.slotIndex, right.cueIndex, right.definitionHash, right.stringHash);
        };
        auto directiveLess = [](const auto& left, const auto& right) {
            return std::tie(left.slotIndex, left.nameHash, left.elementIndex)
                   < std::tie(right.slotIndex, right.nameHash, right.elementIndex);
        };
        std::sort(output.dialogueCueTexts.begin(), output.dialogueCueTexts.end(), dialogueLess);
        std::sort(output.directiveElements.begin(), output.directiveElements.end(), directiveLess);
        output.dialogueCueTexts.erase(std::unique(output.dialogueCueTexts.begin(),
                                                  output.dialogueCueTexts.end(),
                                                  [&](const auto& left, const auto& right) {
                                                      return !dialogueLess(left, right)
                                                             && !dialogueLess(right, left);
                                                  }),
                                      output.dialogueCueTexts.end());
        output.directiveElements.erase(std::unique(output.directiveElements.begin(),
                                                   output.directiveElements.end(),
                                                   [&](const auto& left, const auto& right) {
                                                       return !directiveLess(left, right)
                                                              && !directiveLess(right, left);
                                                   }),
                                       output.directiveElements.end());
        return true;
    } catch (...) {
        return false;
    }
}

/** Resolves the native type-53 authored list and attaches its exact bound to the SDK slot row. */
[[nodiscard]] bool attach_dialogue_cue_counts(const topology_inventory::Snapshot& topology,
                                              const squads::Facts& facts,
                                              PackageContext& packageContext,
                                              topology_enrichment::Snapshot& enrichment) {
    if (topology.slots.size() != enrichment.slots.size()) {
        return false;
    }
    struct CachedTag final {
        std::vector<std::byte> bytes{};
        std::uint32_t classId{};
    };
    std::unordered_map<std::uint32_t, CachedTag> cache{};
    auto package = [&](std::uint32_t tag, const CachedTag*& output) -> bool {
        output = nullptr;
        const auto found = cache.find(tag);
        if (found != cache.end()) {
            output = &found->second;
            return true;
        }
        CachedTag row{};
        if (tag == 0 || tag == format::kAbsentIndex
            || !read_tag(&packageContext, tag, row.bytes, row.classId)) {
            return false;
        }
        const auto [inserted, accepted] = cache.emplace(tag, std::move(row));
        if (!accepted) {
            return false;
        }
        output = &inserted->second;
        return true;
    };

    try {
        cache.reserve(facts.descriptors.size());
        for (std::uint32_t slotRow = 0; slotRow < topology.slots.size(); ++slotRow) {
            if (topology.slots[slotRow].slotType != format::kDialogueSlotType) {
                continue;
            }
            topology_enrichment::Slot& enriched = enrichment.slots[slotRow];
            if (enriched.componentClass != format::kDialogueComponentClass
                || enriched.authSchema != format::kDialogueAuthSchema
                || (enriched.flags & format::kSlotSchemaJoinExact) == 0) {
                continue;
            }
            std::uint64_t agreedCount = 0;
            bool sawDescriptor = false;
            bool resolved = true;
            for (const squads::DescriptorFact& descriptor : facts.descriptors) {
                if (descriptor.slotIndex != slotRow) {
                    continue;
                }
                sawDescriptor = true;
                const CachedTag* config = nullptr;
                if (!package(descriptor.configTag, config) || config == nullptr) {
                    resolved = false;
                    break;
                }
                const std::size_t listField = static_cast<std::size_t>(descriptor.descriptorOffset)
                                              + format::kDialogueAuthoredListRelativeOffset;
                std::uint32_t listTag = 0;
                if (!read_value(std::span(config->bytes), listField, listTag) || listTag == 0
                    || listTag == format::kAbsentIndex) {
                    resolved = false;
                    break;
                }
                const CachedTag* authored = nullptr;
                std::uint64_t count = 0;
                if (!package(listTag, authored) || authored == nullptr
                    || !read_value(std::span(authored->bytes), 8U, count) || count == 0
                    || count > format::kDialogueMaximumCueCount
                    || (agreedCount != 0 && agreedCount != count)) {
                    resolved = false;
                    break;
                }
                agreedCount = count;
            }
            if (cancelled(packageContext.cancel, packageContext.cancelContext)) {
                return false;
            }
            if (resolved && sawDescriptor && agreedCount != 0) {
                enriched.dialogueCueCount = static_cast<std::uint32_t>(agreedCount);
                enriched.flags |= format::kSlotDialogueCuesExact;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

/** Builds one schema fact for every final catalog-global slot row. */
[[nodiscard]] bool build_slot_schemas(const topology_inventory::Snapshot& topology,
                                      const topology_enrichment::Snapshot& enrichment,
                                      std::vector<squads::SlotSchemaFact>& output) {
    output.clear();
    if (topology.slots.size() != enrichment.slots.size()
        || topology.slots.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    try {
        output.reserve(topology.slots.size());
        for (std::uint32_t index = 0; index < topology.slots.size(); ++index) {
            const topology_enrichment::Slot& slot = enrichment.slots[index];
            output.push_back({index,
                              slot.componentClass,
                              slot.senseSchema,
                              slot.authSchema,
                              (slot.flags & format::kSlotSchemaJoinExact) != 0});
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Resolves one exact actor definition tag against the validated sorted actor section. */
[[nodiscard]] bool
resolve_actor(void* opaque, std::uint32_t definitionTag, std::uint32_t& output) noexcept {
    output = format::kAbsentIndex;
    if (opaque == nullptr) {
        return false;
    }
    const auto& actors = *static_cast<const std::vector<actor_rsat::ActorClass>*>(opaque);
    const auto found = std::lower_bound(actors.begin(),
                                        actors.end(),
                                        definitionTag,
                                        [](const actor_rsat::ActorClass& row, std::uint32_t tag) {
                                            return row.definitionTag < tag;
                                        });
    if (found == actors.end() || found->definitionTag != definitionTag
        || static_cast<std::size_t>(found - actors.begin()) >= format::kAbsentIndex) {
        return false;
    }
    output = static_cast<std::uint32_t>(found - actors.begin());
    return true;
}

/** Converts the runtime trust identity to the writer's header-only identity. */
[[nodiscard]] pack::Identity
pack_identity(const state::activity_sdk::identity::Expected& identity) noexcept {
    return {identity.sdkBuildSha256, identity.contentKeySha256, identity.logicalIrSha256};
}

/** Borrows all declaration-bearing final sections for one Lua artifact transaction. */
[[nodiscard]] lua::Source
lua_source(const state::activity_sdk::identity::Expected& identity,
           const composer::Storage& storage,
           std::span<const lua::ScenarioWorldSource> worldSources) noexcept {
    return {identity.sdkBuildSha256,
            identity.payloadSha256,
            identity.contentKeySha256,
            identity.logicalIrSha256,
            storage.strings,
            storage.activities,
            storage.scenarios,
            storage.bubbles,
            storage.states,
            storage.objects,
            storage.occurrences,
            storage.slots,
            storage.squads,
            storage.squadMembers,
            storage.squadAnchors,
            storage.authoredSceneResources,
            storage.authoredSceneSquadEdges,
            storage.taskTargets,
            storage.dialogueCueTexts,
            storage.directiveElements,
            storage.behaviorPrograms,
            storage.behaviorInputs,
            storage.behaviorChannelWrites,
            storage.behaviorOwners,
            storage.behaviorActivityBindings,
            storage.actorClasses,
            storage.actorMessageSchemas,
            storage.actorCommandDefinitions,
            storage.actorBehaviorProfiles,
            storage.simulationEventDefinitions,
            storage.runtimeSchemas,
            storage.runtimeFields,
            storage.runtimeTypeDefinitions,
            storage.sobjectRsats,
            storage.sobjectRsatDescriptors,
            storage.entityTypeDefinitions,
            storage.sobjectRsatFieldBindings,
            storage.actorStateNames,
            worldSources};
}

} // namespace

/** Returns the stable log token for each pipeline result. */
const char* status_name(Status value) noexcept {
    switch (value) {
    case Status::ready:
        return "ready";
    case Status::cancelled:
        return "cancelled";
    case Status::invalidInput:
        return "invalid_input";
    case Status::activityEnrichment:
        return "activity_enrichment";
    case Status::topologyEnrichment:
        return "topology_enrichment";
    case Status::squadFacts:
        return "squad_facts";
    case Status::actorRsat:
        return "actor_rsat";
    case Status::squadLink:
        return "squad_link";
    case Status::authoredSceneFacts:
        return "authored_scene_facts";
    case Status::authoredSceneLinks:
        return "authored_scene_links";
    case Status::dialogueCues:
        return "dialogue_cues";
    case Status::authoredText:
        return "authored_text";
    case Status::behaviors:
        return "behaviors";
    case Status::policyInputs:
        return "policy_inputs";
    case Status::policy:
        return "policy";
    case Status::composition:
        return "composition";
    case Status::identity:
        return "identity";
    case Status::luaBuild:
        return "lua_build";
    case Status::publication:
        return "publication";
    case Status::luaPublication:
        return "lua_publication";
    case Status::reload:
        return "reload";
    }
    return "invalid_input";
}

/** Builds and publishes one canonical native SDK generation without a runtime reload. */
Status stage(const wchar_t* sdkDirectory,
             const wchar_t* packPath,
             const reader::Source& source,
             const pack::Digest& sourceFingerprint,
             const activity_inventory::Snapshot& activities,
             topology_inventory::Snapshot& topology,
             std::span<const lua::ScenarioWorldSource> scenarioWorldSources,
             const external_placements::Index& externalPlacements,
             bool luaDeclarations,
             CancelProbe cancel,
             void* cancelContext,
             ProgressProbe progress,
             void* progressContext,
             Result& output) noexcept {
    output = {};
    if (sdkDirectory == nullptr || sdkDirectory[0] == L'\0' || packPath == nullptr
        || packPath[0] == L'\0' || source.directory.empty() || source.keys == nullptr
        || !state::activity_sdk::identity::valid(sourceFingerprint) || !topology.ready) {
        return Status::invalidInput;
    }
    if (cancelled(cancel, cancelContext)) {
        return Status::cancelled;
    }
    try {
        PackageContext packageContext{
            &source,
            std::unique_ptr<reader::Scratch>(new (std::nothrow) reader::Scratch()),
            cancel,
            cancelContext};
        if (packageContext.scratch == nullptr || cancelled(cancel, cancelContext)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled
                                                    : Status::activityEnrichment;
        }
        report(progress, progressContext, Phase::activityMetadata);
        activity_enrichment::Snapshot activityNames{};
        if (!activity_enrichment::build(source, *packageContext.scratch, activities, activityNames)
            || !activity_enrichment::apply(activityNames, topology)) {
            return Status::activityEnrichment;
        }

        report(progress, progressContext, Phase::worldTopology);
        topology_enrichment::Snapshot topologyDetails{};
        if (!topology_enrichment::build_generated(topology, topologyDetails)) {
            return Status::topologyEnrichment;
        }
        if (cancelled(cancel, cancelContext)) {
            return Status::cancelled;
        }

        report(progress, progressContext, Phase::squadFacts);
        squads::Facts squadFacts{};
        if (!squads::collect_facts(topology, &read_tag, &packageContext, squadFacts)
            || !squads::append_external_placements(topology, externalPlacements, squadFacts)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::squadFacts;
        }

        report(progress, progressContext, Phase::actorDefinitions);
        actor_rsat::Snapshot actorRows{};
        std::vector<std::uint32_t> installedRsats{};
        reader::ScanResult rsatScan{};
        if (!reader::scan_class(source.directory,
                                actor_rsat::kActorRsatClass,
                                &collect_tag,
                                &installedRsats,
                                rsatScan)) {
            return Status::actorRsat;
        }
        std::sort(installedRsats.begin(), installedRsats.end());
        installedRsats.erase(std::unique(installedRsats.begin(), installedRsats.end()),
                             installedRsats.end());
        if (!actor_rsat::build_with_rsats(source,
                                          squadFacts.actorDefinitionTags,
                                          installedRsats,
                                          cancel,
                                          cancelContext,
                                          actorRows)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::actorRsat;
        }
        report(progress, progressContext, Phase::squadLinks);
        std::vector<squads::SlotSchemaFact> slotSchemas{};
        if (!build_slot_schemas(topology, topologyDetails, slotSchemas)) {
            return Status::squadLink;
        }
        squadFacts.slotSchemas = std::move(slotSchemas);
        squads::Snapshot squadRows{};
        if (!squads::link(
                topology, squadFacts, &resolve_actor, &actorRows.actorClasses, squadRows)) {
            return Status::squadLink;
        }
        report(progress, progressContext, Phase::authoredSceneFacts);
        authored_scene::Facts sceneFacts{};
        if (!authored_scene::derive_facts(topology, squadFacts, sceneFacts)) {
            return Status::authoredSceneFacts;
        }
        report(progress, progressContext, Phase::authoredSceneLinks);
        authored_scene::Snapshot sceneRows{};
        if (!authored_scene::build(topology, sceneFacts, &read_tag, &packageContext, sceneRows)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled
                                                    : Status::authoredSceneLinks;
        }
        report(progress, progressContext, Phase::dialogueCues);
        if (!attach_dialogue_cue_counts(topology, squadFacts, packageContext, topologyDetails)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::dialogueCues;
        }
        report(progress, progressContext, Phase::authoredText);
        if (!attach_authored_text(topology, squadFacts, packageContext, sceneRows)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::authoredText;
        }
        if (cancelled(cancel, cancelContext)) {
            return Status::cancelled;
        }

        report(progress, progressContext, Phase::behaviors);
        behaviors::Snapshot behaviorRows{};
        if (!behaviors::build(
                source, squadFacts.actorDefinitionTags, cancel, cancelContext, behaviorRows)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::behaviors;
        }

        report(progress, progressContext, Phase::actionPolicies);
        policy_adapter::Snapshot policyInputs{};
        if (!policy_adapter::build(activities, topology, topologyDetails, policyInputs)) {
            return Status::policyInputs;
        }
        policy::Snapshot policyRows{};
        if (!policy::build(policyInputs.view(), policyRows)) {
            return Status::policy;
        }
        if (cancelled(cancel, cancelContext)) {
            return Status::cancelled;
        }

        report(progress, progressContext, Phase::packTables);
        const composer::Inputs inputs{&activities,
                                      &activityNames,
                                      &topology,
                                      &topologyDetails,
                                      &policyRows,
                                      &actorRows,
                                      &squadFacts,
                                      &squadRows,
                                      &sceneRows,
                                      &behaviorRows};
        composer::Storage storage{};
        if (!composer::compose_generated(inputs, storage)) {
            return Status::composition;
        }
        pack::PreparedImage image{};
        if (pack::prepare(storage.tables(), image) != pack::Status::ready) {
            return Status::composition;
        }
        const pack::Digest payload = image.payload_sha256();
        const std::uint64_t fileBytes = image.file_size();
        state::activity_sdk::identity::Expected identity{};
        if (!state::activity_sdk::identity::derive(sourceFingerprint, payload, identity)) {
            return Status::identity;
        }
        if (cancelled(cancel, cancelContext)) {
            return Status::cancelled;
        }

        lua::Bundle luaBundle{};
        if (luaDeclarations) {
            report(progress, progressContext, Phase::luaDeclarations);
            if (lua::build(lua_source(identity, storage, scenarioWorldSources), luaBundle)
                != lua::Status::ready) {
                return Status::luaBuild;
            }
        }

        report(progress, progressContext, Phase::outputFiles);
        pack::Digest written{};
        if (pack::publish(packPath, pack_identity(identity), std::move(image), written)
            != pack::Status::ready) {
            return Status::publication;
        }
        lua::Result luaResult{};
        // The commit renames a lua directory whether or not it holds declarations.
        if (!luaDeclarations) {
            if (!ensure_lua_directory(sdkDirectory)) {
                return Status::luaPublication;
            }
        } else {
            if (lua::publish_bundle(sdkDirectory, luaBundle, luaResult) != lua::Status::ready) {
                return Status::luaPublication;
            }
        }
        output.identity = identity;
        output.payloadSha256 = payload;
        output.fileBytes = fileBytes;
        output.luaBytes = luaResult.byteCount;
        output.luaFiles = luaResult.fileCount;
        return Status::ready;
    } catch (...) {
        output = {};
        return cancelled(cancel, cancelContext) ? Status::cancelled : Status::invalidInput;
    }
}

/** Builds, publishes, and reloads one canonical native SDK generation. */
Status publish(void* module,
               const wchar_t* sdkDirectory,
               const wchar_t* packPath,
               const reader::Source& source,
               const pack::Digest& sourceFingerprint,
               const activity_inventory::Snapshot& activities,
               topology_inventory::Snapshot& topology,
               const external_placements::Index& externalPlacements,
               bool luaDeclarations,
               CancelProbe cancel,
               void* cancelContext,
               Result& output) noexcept {
    output = {};
    if (module == nullptr) {
        return Status::invalidInput;
    }
    const Status staged = stage(sdkDirectory,
                                packPath,
                                source,
                                sourceFingerprint,
                                activities,
                                topology,
                                {},
                                externalPlacements,
                                luaDeclarations,
                                cancel,
                                cancelContext,
                                nullptr,
                                nullptr,
                                output);
    if (staged != Status::ready) {
        return staged;
    }
    if (!state::activity_sdk::reload(module, output.identity)) {
        output = {};
        return Status::reload;
    }
    // A reload replaces the catalog the wire descriptors borrow, so they are rebuilt here.
    return Status::ready;
}

} // namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline
