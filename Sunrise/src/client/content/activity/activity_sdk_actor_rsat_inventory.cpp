#include "activity_sdk_actor_rsat_inventory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../../../middleware/content/packages/reader/parallel.h"

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {
namespace {

namespace format = state::activity_sdk::format;
namespace reader = middleware::content::packages::reader;

/** Exact Tiger typed-array marker and the canonical extraction count ceiling. */
constexpr std::uint32_t kArrayMarker = 0x80809FBDU;
constexpr std::int64_t kMaximumArrayCount = 1'000'000;
/** Zero and the all-ones sentinel do not name a package row. */
constexpr std::uint32_t kAbsentTag = 0xFFFFFFFFU;
/** An actor definition reaches its state-machine definition through a placed object. */
constexpr std::uint32_t kPlacedObjectClass = 0x80809C36U;

#include "activity_sdk_actor_engine_semantics.inc"

/** One validated typed-array descriptor and its exact stored metadata. */
struct TypedArray final {
    std::uint32_t count{};
    std::int64_t relative{};
    std::uint32_t headerOffset{format::kAbsentIndex};
    std::uint32_t dataOffset{format::kAbsentIndex};
    std::uint32_t elementClass{format::kAbsentIndex};
    bool typed{};
};

/** One schema and its raw fields before global schema ordering is known. */
struct SchemaSource final {
    RsatSchema row{};
    std::vector<RsatField> fields{};
};

/** State owned by one transactional build. */
struct BuildState final {
    ReadTag readTag{};
    void* readContext{};
    CancelProbe cancel{};
    void* cancelContext{};
    Snapshot snapshot{};
    std::vector<SchemaSource> schemaSources{};
    std::unordered_map<std::uint32_t, std::size_t> schemaIndexes{};
    std::unordered_set<std::uint32_t> rsatTags{};
};

/** Heap-owned package reader storage is always closed before release. */
class ScratchOwner final {
public:
    ScratchOwner() noexcept : value_(new(std::nothrow) reader::Scratch()) {}

    ~ScratchOwner() noexcept {
        if (value_ != nullptr) {
            reader::close_files(*value_);
        }
    }

    ScratchOwner(const ScratchOwner&) = delete;
    ScratchOwner& operator=(const ScratchOwner&) = delete;

    [[nodiscard]] reader::Scratch* get() const noexcept {
        return value_.get();
    }

private:
    std::unique_ptr<reader::Scratch> value_{};
};

/** Actual reader callback context. */
struct PrefetchedTag final {
    std::vector<std::byte> bytes{};
    std::uint32_t classId{};
};

struct PackageReadContext final {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    std::unordered_map<std::uint32_t, PrefetchedTag> prefetched{};
};

/** Releases the process-wide parallel package readers after one inventory build. */
class ParallelReadOwner final {
public:
    ~ParallelReadOwner() noexcept {
        reader::parallel::release();
    }

    ParallelReadOwner() = default;
    ParallelReadOwner(const ParallelReadOwner&) = delete;
    ParallelReadOwner& operator=(const ParallelReadOwner&) = delete;
};

[[nodiscard]] bool is_cancelled(CancelProbe probe, void* context) noexcept {
    return probe != nullptr && probe(context);
}

[[nodiscard]] bool is_absent_tag(std::uint32_t tag) noexcept {
    return tag == 0 || tag == kAbsentTag;
}

/** @return True when one complete range lies in the package blob. */
[[nodiscard]] bool
contains(std::span<const std::byte> blob, std::size_t offset, std::size_t size) noexcept {
    return offset <= blob.size() && size <= blob.size() - offset;
}

template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> blob, std::size_t offset, Value& output) noexcept {
    output = {};
    if (!contains(blob, offset, sizeof output)) {
        return false;
    }
    std::memcpy(&output, blob.data() + offset, sizeof output);
    return true;
}

[[nodiscard]] bool to_u32(std::size_t value, std::uint32_t& output) noexcept {
    output = 0;
    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

/** Adds one signed self-relative field without wrapping host arithmetic. */
[[nodiscard]] bool
relative_offset(std::size_t base, std::int64_t relative, std::size_t& output) noexcept {
    constexpr std::int64_t kMaximum = (std::numeric_limits<std::int64_t>::max)();
    constexpr std::int64_t kMinimum = (std::numeric_limits<std::int64_t>::min)();
    output = 0;
    if (base > static_cast<std::size_t>(kMaximum)) {
        return false;
    }
    const auto signedBase = static_cast<std::int64_t>(base);
    if ((relative > 0 && signedBase > kMaximum - relative)
        || (relative < 0 && signedBase < kMinimum - relative)) {
        return false;
    }
    const std::int64_t target = signedBase + relative;
    if (target < 0) {
        return false;
    }
    output = static_cast<std::size_t>(target);
    return true;
}

/** Reads the exact `{i64 count, i64 self-relative header}` package array. */
[[nodiscard]] bool typed_array(std::span<const std::byte> blob,
                               std::size_t field,
                               std::uint32_t expectedClass,
                               std::size_t stride,
                               TypedArray& output) noexcept {
    output = {};
    std::int64_t count = 0;
    std::int64_t relative = 0;
    if (!read_value(blob, field, count) || !read_value(blob, field + 8U, relative) || count < 0
        || count > kMaximumArrayCount) {
        return false;
    }
    output.count = static_cast<std::uint32_t>(count);
    output.relative = relative;
    if (count == 0 && relative == 0) {
        return true;
    }

    std::size_t header = 0;
    if (!relative_offset(field + 8U, relative, header) || header < 4U
        || !contains(blob, header - 4U, 20U)) {
        return false;
    }
    std::uint32_t marker = 0;
    std::int64_t repeatedCount = 0;
    std::uint32_t elementClass = 0;
    std::uint32_t padding = 0;
    if (!read_value(blob, header - 4U, marker) || marker != kArrayMarker
        || !read_value(blob, header, repeatedCount) || repeatedCount != count
        || !read_value(blob, header + 8U, elementClass) || elementClass != expectedClass
        || !read_value(blob, header + 12U, padding) || padding != 0
        || header > (std::numeric_limits<std::size_t>::max)() - 16U) {
        return false;
    }
    const std::size_t data = header + 16U;
    const auto unsignedCount = static_cast<std::uint64_t>(count);
    if (data > blob.size()
        || (unsignedCount != 0
            && unsignedCount > static_cast<std::uint64_t>((blob.size() - data) / stride))
        || !to_u32(header, output.headerOffset) || !to_u32(data, output.dataOffset)) {
        return false;
    }
    output.elementClass = elementClass;
    output.typed = true;
    return true;
}

/** Reads one package array whose element class is not checked, only its marker and count. */
[[nodiscard]] bool untyped_array(std::span<const std::byte> blob,
                                 std::size_t field,
                                 std::size_t stride,
                                 TypedArray& output) noexcept {
    output = {};
    std::int64_t count = 0;
    std::int64_t relative = 0;
    if (!read_value(blob, field, count) || !read_value(blob, field + 8U, relative) || count < 0
        || count > kMaximumArrayCount) {
        return false;
    }
    output.count = static_cast<std::uint32_t>(count);
    output.relative = relative;
    if (count == 0 && relative == 0) {
        return true;
    }
    std::size_t header = 0;
    std::uint32_t marker = 0;
    std::int64_t repeatedCount = 0;
    if (!relative_offset(field + 8U, relative, header) || header < 4U
        || !contains(blob, header - 4U, 20U) || !read_value(blob, header - 4U, marker)
        || marker != kArrayMarker || !read_value(blob, header, repeatedCount)
        || repeatedCount != count || !read_value(blob, header + 8U, output.elementClass)
        || header > (std::numeric_limits<std::size_t>::max)() - 16U) {
        return false;
    }
    const std::size_t data = header + 16U;
    const auto unsignedCount = static_cast<std::uint64_t>(count);
    if (data > blob.size()
        || (unsignedCount != 0
            && unsignedCount > static_cast<std::uint64_t>((blob.size() - data) / stride))
        || !to_u32(header, output.headerOffset) || !to_u32(data, output.dataOffset)) {
        return false;
    }
    output.typed = true;
    return true;
}

/** Every aligned word in the package tag range; a reference field holds its tag verbatim. */
void tag_shaped_words(std::span<const std::byte> blob, std::vector<std::uint32_t>& output) {
    constexpr std::uint32_t kFirstTag = 0x80800000U;
    constexpr std::uint32_t kLastTag = 0x81C00000U;
    output.clear();
    for (std::size_t offset = 0; offset + 4U <= blob.size(); offset += 4U) {
        std::uint32_t word = 0;
        std::memcpy(&word, blob.data() + offset, sizeof word);
        if (word >= kFirstTag && word < kLastTag
            && std::find(output.begin(), output.end(), word) == output.end()) {
            output.push_back(word);
        }
    }
}

/** The state-machine definition lists 32-byte groups at +32; group +12 is the group name hash. */
constexpr std::size_t kStateMachineGroupArrayOffset = 32;
constexpr std::size_t kStateMachineGroupStride = 32;
constexpr std::size_t kStateMachineGroupHashOffset = 12;
constexpr std::size_t kStateMachineGroupNamesOffset = 16;

/** Appends the `state_machine` group's names from one definition, in authored order. */
[[nodiscard]] bool state_machine_names(std::span<const std::byte> definition,
                                       std::vector<std::uint32_t>& output) noexcept {
    output.clear();
    TypedArray groups{};
    if (!untyped_array(
            definition, kStateMachineGroupArrayOffset, kStateMachineGroupStride, groups)) {
        return false;
    }
    bool found = false;
    for (std::uint32_t index = 0; index < groups.count; ++index) {
        const std::size_t element = static_cast<std::size_t>(groups.dataOffset)
                                    + static_cast<std::size_t>(index) * kStateMachineGroupStride;
        std::uint32_t groupHash = 0;
        if (!read_value(definition, element + kStateMachineGroupHashOffset, groupHash)) {
            return false;
        }
        if (groupHash != format::kActorStateMachineGroupHash) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        TypedArray names{};
        if (!untyped_array(definition, element + kStateMachineGroupNamesOffset, 4U, names)) {
            return false;
        }
        for (std::uint32_t ordinal = 0; ordinal < names.count; ++ordinal) {
            std::uint32_t nameHash = 0;
            if (!read_value(definition,
                            static_cast<std::size_t>(names.dataOffset)
                                + static_cast<std::size_t>(ordinal) * 4U,
                            nameHash)) {
                return false;
            }
            output.push_back(nameHash);
        }
    }
    return found;
}

/**
 * Collects the state names one actor class can be told to enter. The actor definition refers to
 * placed objects, and exactly one of those refers to one state-machine definition. Any other
 * shape emits no rows for the class.
 */
[[nodiscard]] bool collect_state_names(BuildState& state,
                                       std::uint32_t actorIndex,
                                       std::span<const std::byte> actorBlob) {
    std::vector<std::uint32_t> words{};
    std::vector<std::uint32_t> placedWords{};
    std::vector<std::byte> placed{};
    std::vector<std::byte> definition{};
    std::vector<std::uint32_t> definitions{};
    tag_shaped_words(actorBlob, words);
    for (const std::uint32_t placedTag : words) {
        if (!state.readTag(state.readContext, placedTag, kPlacedObjectClass, placed)) {
            continue;
        }
        tag_shaped_words(placed, placedWords);
        for (const std::uint32_t definitionTag : placedWords) {
            if (std::find(definitions.begin(), definitions.end(), definitionTag)
                    != definitions.end()
                || !state.readTag(state.readContext,
                                  definitionTag,
                                  format::kActorStateMachineDefinitionClass,
                                  definition)) {
                continue;
            }
            definitions.push_back(definitionTag);
        }
    }
    if (definitions.size() != 1
        || !state.readTag(state.readContext,
                          definitions.front(),
                          format::kActorStateMachineDefinitionClass,
                          definition)) {
        return true;
    }
    std::vector<std::uint32_t> names{};
    if (!state_machine_names(definition, names)) {
        return true;
    }
    // The ordinal counts emitted rows, not authored positions. A skipped name would otherwise
    // leave a gap, and the catalog requires one contiguous run per actor class.
    std::uint32_t ordinal = 0;
    for (const std::uint32_t nameHash : names) {
        if (nameHash == 0 || nameHash == kAbsentTag) {
            continue;
        }
        ActorStateName row{};
        row.actorClassIndex = actorIndex;
        row.definitionTag = definitions.front();
        row.groupHash = format::kActorStateMachineGroupHash;
        row.nameHash = nameHash;
        row.ordinal = ordinal++;
        row.flags = format::kActorStateNameExact;
        state.snapshot.actorStateNames.push_back(row);
    }
    return true;
}

/** Formats the stable actor-class ID used by the generated pack. */
[[nodiscard]] bool format_actor_id(std::uint32_t tag, Text& output) noexcept {
    output = {};
    const int written = std::snprintf(
        output.value.data(), output.value.size(), "actor-class/%08x", static_cast<unsigned>(tag));
    if (written <= 0 || static_cast<std::size_t>(written) >= output.value.size()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(written);
    return true;
}

/** Formats the stable RSAT schema ID used by the generated pack. */
[[nodiscard]] bool format_schema_id(std::uint32_t tag, Text& output) noexcept {
    output = {};
    const int written = std::snprintf(
        output.value.data(), output.value.size(), "rsat-schema/%08x", static_cast<unsigned>(tag));
    if (written <= 0 || static_cast<std::size_t>(written) >= output.value.size()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(written);
    return true;
}

/** Formats the stable tag-and-ordinal ID for one RSAT descriptor. */
[[nodiscard]] bool
format_descriptor_id(std::uint32_t tag, std::uint32_t ordinal, Text& output) noexcept {
    output = {};
    const int written = std::snprintf(output.value.data(),
                                      output.value.size(),
                                      "rsat-descriptor/%08x/%06x",
                                      static_cast<unsigned>(tag),
                                      static_cast<unsigned>(ordinal));
    if (written <= 0 || static_cast<std::size_t>(written) >= output.value.size()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(written);
    return true;
}

/** Copies one bounded catalog name into fixed inventory storage. */
[[nodiscard]] bool text_literal(std::string_view value, Text& output) noexcept {
    output = {};
    if (value.empty() || value.size() >= output.value.size()) {
        return false;
    }
    std::copy(value.begin(), value.end(), output.value.begin());
    output.length = static_cast<std::uint16_t>(value.size());
    return true;
}

/** Adds the pinned actor command semantics shared by every installed actor. */
[[nodiscard]] bool add_engine_semantics(Snapshot& snapshot) {
    const ExtractedActorEngineSemantics& extracted = kExtractedActorEngineSemantics;
    const bool hasIdentity = std::any_of(extracted.executableIdentity.begin(),
                                         extracted.executableIdentity.end(),
                                         [](std::byte value) { return value != std::byte{}; });
    if (extracted.version == 0 || !hasIdentity || extracted.message.evidenceAddress == 0) {
        return false;
    }
    ActorMessageSchema message{};
    if (!text_literal(extracted.message.name, message.name)) {
        return false;
    }
    message.definitionHandle = extracted.message.definitionHandle;
    message.durableKey = extracted.message.durableKey;
    message.ownerClass = extracted.message.ownerClass;
    message.handlerSlot = extracted.message.handlerSlot;
    message.bodyType = extracted.message.bodyType;
    message.commands = {0, static_cast<std::uint32_t>(extracted.commands.size())};
    message.flags = format::kActorMessageSchemaExact;
    snapshot.messageSchemas.push_back(message);
    for (const ExtractedActorCommandDefinition& input : extracted.commands) {
        ActorCommandDefinition command{};
        if (input.metadataEvidenceAddress == 0 || !text_literal(input.name, command.name)) {
            return false;
        }
        command.selector = input.selector;
        command.payloadHandle = input.payloadHandle;
        command.effect = input.setFaction ? format::ActorCommandEffect::setFaction
                                          : format::ActorCommandEffect::opaque;
        if (input.setFaction) {
            if (input.effectEvidenceAddress == 0
                || !text_literal(input.factionNoneName, command.factionNoneName)
                || !text_literal(input.factionRemovedName, command.factionRemovedName)
                || !text_literal(input.factionHostileToAllName, command.factionHostileToAllName)) {
                return false;
            }
            command.factionNone = input.factionNone;
            command.factionRemoved = input.factionRemoved;
            command.factionHostileToAll = input.factionHostileToAll;
        }
        command.flags = format::kActorCommandDefinitionExact;
        snapshot.commandDefinitions.push_back(command);
    }
    for (const ExtractedSimulationEventDefinition& input : extracted.simulationEvents) {
        SimulationEventDefinition event{};
        if (!text_literal(input.name, event.name)) {
            return false;
        }
        event.eventType = input.eventType;
        event.primarySchema = input.primarySchema;
        event.secondarySchema = input.secondarySchema;
        event.descriptorEvidenceAddress = input.descriptorEvidenceAddress;
        event.primaryEvidenceAddress = input.primaryEvidenceAddress;
        event.secondaryEvidenceAddress = input.secondaryEvidenceAddress;
        event.flags = format::kSimulationEventDefinitionExact;
        if (input.primarySchema == format::kAbsentIndex) {
            event.flags |= format::kSimulationEventPrimaryAbsent;
        }
        if (input.secondarySchema == format::kAbsentIndex) {
            event.flags |= format::kSimulationEventSecondaryAbsent;
        }
        snapshot.simulationEvents.push_back(event);
    }
    for (const ExtractedRuntimeSchema& input : extracted.runtimeSchemas) {
        RuntimeSchema schema{};
        schema.handle = input.handle;
        schema.decodedSize = input.decodedSize;
        schema.definitionHash = input.definitionHash;
        schema.definitionClass = input.definitionClass;
        schema.codecFamilies = input.codecFamilies;
        schema.fields = {input.firstField, input.fieldCount};
        schema.evidenceAddress = input.evidenceAddress;
        schema.flags = input.flags;
        schema.arrayElementCount = input.arrayElementCount;
        snapshot.runtimeSchemas.push_back(schema);
    }
    for (const ExtractedRuntimeField& input : extracted.runtimeFields) {
        RuntimeField field{};
        field.schemaIndex = input.schemaIndex;
        field.ordinal = input.ordinal;
        field.structOffset = input.structOffset;
        field.alternateOffset = input.alternateOffset;
        field.typeCode = input.typeCode;
        field.nestedHandle = input.nestedHandle;
        field.bias = input.bias;
        field.bits = input.bits;
        field.codecParameters = input.codecParameters;
        field.flags = input.flags;
        snapshot.runtimeFields.push_back(field);
    }
    for (const ExtractedRuntimeTypeDefinition& input : extracted.runtimeTypes) {
        RuntimeTypeDefinition type{};
        if (!text_literal(input.name, type.name)) {
            return false;
        }
        type.codecFamilies = input.codecFamily;
        type.typeCode = input.typeCode;
        type.decodedSize = input.decodedSize;
        type.fixedBits = input.fixedBits;
        type.minimumBits = input.minimumBits;
        type.maximumBits = input.maximumBits;
        type.writerEvidenceAddress = input.writerEvidenceAddress;
        type.readerEvidenceAddress = input.readerEvidenceAddress;
        type.flags = input.flags;
        snapshot.runtimeTypes.push_back(type);
    }
    for (const ExtractedEntityTypeDefinition& input : extracted.entityTypes) {
        EntityTypeDefinition entity{};
        if (!text_literal(input.name, entity.name)) {
            return false;
        }
        entity.entityType = input.entityType;
        entity.baselineSchema = input.baselineSchema;
        entity.updateSchema = input.updateSchema;
        entity.vtableEvidenceAddress = input.vtableEvidenceAddress;
        entity.baselineEvidenceAddress = input.baselineEvidenceAddress;
        entity.updateEvidenceAddress = input.updateEvidenceAddress;
        entity.flags = input.flags;
        snapshot.entityTypes.push_back(entity);
    }
    return true;
}

/** Accepts only one bounded string followed by zero-filled storage. */
[[nodiscard]] bool valid_text(const Text& text) noexcept {
    if (text.length >= text.value.size() || text.value[text.length] != '\0') {
        return false;
    }
    for (std::size_t index = 0; index < text.length; ++index) {
        if (text.value[index] == '\0') {
            return false;
        }
    }
    return std::all_of(text.value.begin() + text.length + 1U, text.value.end(), [](char value) {
        return value == '\0';
    });
}

[[nodiscard]] bool same_text(const Text& left, const Text& right) noexcept {
    return valid_text(left) && valid_text(right) && left.length == right.length
           && left.value == right.value;
}

[[nodiscard]] bool range_inside(format::Range range, std::size_t size) noexcept {
    return range.first <= size && range.count <= size - range.first;
}

/** Field-5 lanes 1..4 are bias-one values with widths 2, 3, 2, and 3. */
[[nodiscard]] bool valid_authored_spawn_profile(const ActorClass& actor) noexcept {
    constexpr std::array<std::int8_t, 4> kMaximumLogical{2, 6, 2, 6};
    for (std::size_t index = 0; index < actor.authoredSpawnProfile.size(); ++index) {
        if (actor.authoredSpawnProfile[index] < 0
            || actor.authoredSpawnProfile[index] > kMaximumLogical[index]) {
            return false;
        }
    }
    return true;
}

/** Checks an exact stored self-relative header and data pair. */
[[nodiscard]] bool stored_typed_array(std::uint32_t field,
                                      std::int64_t relative,
                                      std::uint32_t header,
                                      std::uint32_t data) noexcept {
    std::size_t expected = 0;
    return relative != 0
           && relative_offset(static_cast<std::size_t>(field) + 8U, relative, expected)
           && expected <= (std::numeric_limits<std::uint32_t>::max)()
           && header == static_cast<std::uint32_t>(expected)
           && header <= (std::numeric_limits<std::uint32_t>::max)() - 16U && data == header + 16U;
}

/** Reads and retains one schema exactly once by tag. */
[[nodiscard]] bool schema(BuildState& state, std::uint32_t tag, std::size_t& sourceIndex) noexcept {
    sourceIndex = 0;
    if (is_absent_tag(tag) || is_cancelled(state.cancel, state.cancelContext)) {
        return false;
    }
    const auto found = state.schemaIndexes.find(tag);
    if (found != state.schemaIndexes.end()) {
        sourceIndex = found->second;
        return true;
    }

    std::vector<std::byte> blob{};
    if (!state.readTag(state.readContext, tag, format::kActorRsatSchemaClass, blob)) {
        return false;
    }
    TypedArray array{};
    if (!typed_array(blob,
                     format::kActorRsatSchemaFieldArrayOffset,
                     format::kActorRsatSchemaFieldClass,
                     kSchemaFieldStride,
                     array)) {
        return false;
    }

    SchemaSource source{};
    if (!format_schema_id(tag, source.row.id)) {
        return false;
    }
    source.row.schemaTag = tag;
    source.row.schemaClass = format::kActorRsatSchemaClass;
    source.row.fieldCount = array.count;
    source.row.fieldArrayOffset = format::kActorRsatSchemaFieldArrayOffset;
    source.row.fieldArrayRelative = array.relative;
    source.row.fieldArrayHeaderOffset = array.headerOffset;
    source.row.fieldArrayDataOffset = array.dataOffset;
    source.row.fieldElementClass = array.elementClass;
    if (array.typed) {
        source.row.flags |= format::kRsatSchemaTypedFieldArray;
    }
    if (array.count != 0) {
        if (!read_value(blob, array.dataOffset, source.row.firstFieldRuntimeGate)
            || !read_value(blob, array.dataOffset + 0x10U, source.row.firstFieldRawU32At10)) {
            return false;
        }
        if (source.row.firstFieldRuntimeGate != format::kAbsentIndex) {
            source.row.flags |= format::kRsatSchemaDynamicPresenceEligible;
        }
    }
    try {
        source.fields.reserve(array.count);
        for (std::uint32_t ordinal = 0; ordinal < array.count; ++ordinal) {
            const std::size_t offset = static_cast<std::size_t>(array.dataOffset)
                                       + static_cast<std::size_t>(ordinal) * kSchemaFieldStride;
            RsatField field{};
            std::copy_n(blob.data() + offset, field.rawRow.size(), field.rawRow.begin());
            source.fields.push_back(field);
        }
        sourceIndex = state.schemaSources.size();
        state.schemaSources.push_back(std::move(source));
        state.schemaIndexes.emplace(tag, sourceIndex);
        return true;
    } catch (...) {
        return false;
    }
}

/** Reads one installed SObject RSAT and retains its complete ordered component layout. */
[[nodiscard]] bool sobject_rsat(BuildState& state, std::uint32_t tag) noexcept {
    if (is_absent_tag(tag) || is_cancelled(state.cancel, state.cancelContext)
        || state.snapshot.sobjectRsats.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    std::vector<std::byte> blob{};
    TypedArray array{};
    SobjectRsat row{};
    if (!state.readTag(state.readContext, tag, kActorRsatClass, blob)
        || !read_value(blob, kActorRsatReverseDefinitionOffset, row.reverseDefinitionTag)
        || !typed_array(blob,
                        format::kActorRsatDescriptorArrayOffset,
                        format::kActorRsatDescriptorClass,
                        kDescriptorStride,
                        array)
        || state.snapshot.sobjectRsatDescriptors.size()
               > (std::numeric_limits<std::uint32_t>::max)() - array.count) {
        return false;
    }
    row.rsatTag = tag;
    row.descriptorArrayOffset = format::kActorRsatDescriptorArrayOffset;
    row.descriptorArrayRelative = array.relative;
    row.descriptorArrayHeaderOffset = array.headerOffset;
    row.descriptorArrayDataOffset = array.dataOffset;
    row.descriptorElementClass = array.elementClass;
    row.provenance = format::ActorSemanticProvenance::packageField;
    row.descriptors.first =
        static_cast<std::uint32_t>(state.snapshot.sobjectRsatDescriptors.size());
    row.descriptors.count = array.count;
    row.flags = format::kSobjectRsatExact;
    const std::uint32_t rsatIndex = static_cast<std::uint32_t>(state.snapshot.sobjectRsats.size());
    std::uint32_t tailOrdinal = 0;
    for (std::uint32_t ordinal = 0; ordinal < array.count; ++ordinal) {
        const std::size_t offset = static_cast<std::size_t>(array.dataOffset)
                                   + static_cast<std::size_t>(ordinal) * kDescriptorStride;
        SobjectRsatDescriptor descriptor{};
        descriptor.rsatIndex = rsatIndex;
        descriptor.descriptorOrdinal = ordinal;
        if (!to_u32(offset, descriptor.descriptorOffset)) {
            return false;
        }
        std::copy_n(blob.data() + offset, descriptor.rawRow.size(), descriptor.rawRow.begin());
        std::memcpy(
            &descriptor.componentTag, descriptor.rawRow.data(), sizeof descriptor.componentTag);
        std::memcpy(
            &descriptor.schemaTag, descriptor.rawRow.data() + 4U, sizeof descriptor.schemaTag);
        std::size_t schemaSourceIndex = 0;
        if (!schema(state, descriptor.schemaTag, schemaSourceIndex)) {
            return false;
        }
        const RsatSchema& schemaRow = state.schemaSources[schemaSourceIndex].row;
        descriptor.schemaFieldCount = schemaRow.fieldCount;
        descriptor.schemaFirstFieldRuntimeGate = schemaRow.firstFieldRuntimeGate;
        if ((schemaRow.flags & format::kRsatSchemaDynamicPresenceEligible) != 0) {
            descriptor.flags |= format::kSobjectRsatDescriptorDynamicPresenceEligible;
            descriptor.dynamicPresenceTailOrdinal = tailOrdinal++;
        }
        state.snapshot.sobjectRsatDescriptors.push_back(descriptor);
    }
    row.dynamicPresenceTailCount = tailOrdinal;
    state.snapshot.sobjectRsats.push_back(row);
    return true;
}

/** Reads one package row and confirms its physical class. */
[[nodiscard]] bool package_read(void* opaque,
                                std::uint32_t tag,
                                std::uint32_t expectedClass,
                                std::vector<std::byte>& output) noexcept {
    output.clear();
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PackageReadContext*>(opaque);
    const auto prefetched = context.prefetched.find(tag);
    if (prefetched != context.prefetched.end()) {
        if (prefetched->second.classId != expectedClass) {
            return false;
        }
        try {
            output = prefetched->second.bytes;
            return true;
        } catch (...) {
            output.clear();
            return false;
        }
    }
    std::uint32_t actualClass = 0;
    return context.source != nullptr && context.scratch != nullptr
           && reader::read_tag_class(*context.source, *context.scratch, tag, actualClass)
           && actualClass == expectedClass
           && reader::read_tag(*context.source, *context.scratch, tag, output, actualClass);
}

} // namespace

/** Validates every id, scalar, raw row, owner, range, order, join, and flag. */
bool validate(const Snapshot& snapshot) noexcept {
    if (!snapshot.complete || snapshot.actorClasses.empty()) {
        return false;
    }

    Text messageName{};
    const ExtractedActorEngineSemantics& extracted = kExtractedActorEngineSemantics;
    if (snapshot.messageSchemas.size() != 1
        || snapshot.commandDefinitions.size() != extracted.commands.size()
        || snapshot.behaviorProfiles.size() != snapshot.actorClasses.size()
        || !text_literal(extracted.message.name, messageName)) {
        return false;
    }
    const ActorMessageSchema& message = snapshot.messageSchemas.front();
    if (!same_text(message.name, messageName)
        || message.definitionHandle != extracted.message.definitionHandle
        || message.durableKey != extracted.message.durableKey
        || message.ownerClass != extracted.message.ownerClass
        || message.handlerSlot != extracted.message.handlerSlot
        || message.bodyType != extracted.message.bodyType
        || message.provenance != format::ActorSemanticProvenance::executableStatic
        || message.commands.first != 0
        || message.commands.count != snapshot.commandDefinitions.size()
        || message.flags != format::kActorMessageSchemaExact) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.commandDefinitions.size(); ++index) {
        const ActorCommandDefinition& command = snapshot.commandDefinitions[index];
        const ExtractedActorCommandDefinition& expected = extracted.commands[index];
        Text commandName{};
        Text factionNoneName{};
        Text factionRemovedName{};
        Text factionHostileName{};
        const auto expectedEffect = expected.setFaction ? format::ActorCommandEffect::setFaction
                                                        : format::ActorCommandEffect::opaque;
        if (!text_literal(expected.name, commandName)
            || (expected.setFaction
                && (!text_literal(expected.factionNoneName, factionNoneName)
                    || !text_literal(expected.factionRemovedName, factionRemovedName)
                    || !text_literal(expected.factionHostileToAllName, factionHostileName)))
            || !same_text(command.name, commandName)
            || !same_text(command.factionNoneName, factionNoneName)
            || !same_text(command.factionRemovedName, factionRemovedName)
            || !same_text(command.factionHostileToAllName, factionHostileName)
            || command.selector != expected.selector
            || command.payloadHandle != expected.payloadHandle || command.effect != expectedEffect
            || command.provenance != format::ActorSemanticProvenance::executableStatic
            || command.factionNone != expected.factionNone
            || command.factionRemoved != expected.factionRemoved
            || command.factionHostileToAll != expected.factionHostileToAll
            || command.flags != format::kActorCommandDefinitionExact) {
            return false;
        }
    }
    if (snapshot.simulationEvents.size() != extracted.simulationEvents.size()
        || snapshot.runtimeSchemas.size() != extracted.runtimeSchemas.size()
        || snapshot.runtimeFields.size() != extracted.runtimeFields.size()
        || snapshot.runtimeTypes.size() != extracted.runtimeTypes.size()
        || snapshot.entityTypes.size() != extracted.entityTypes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.simulationEvents.size(); ++index) {
        const SimulationEventDefinition& row = snapshot.simulationEvents[index];
        const ExtractedSimulationEventDefinition& expected = extracted.simulationEvents[index];
        Text expectedName{};
        const std::uint32_t absentFlags =
            (expected.primarySchema == format::kAbsentIndex ? format::kSimulationEventPrimaryAbsent
                                                            : 0U)
            | (expected.secondarySchema == format::kAbsentIndex
                   ? format::kSimulationEventSecondaryAbsent
                   : 0U);
        if (!text_literal(expected.name, expectedName) || !same_text(row.name, expectedName)
            || row.eventType != expected.eventType || row.primarySchema != expected.primarySchema
            || row.secondarySchema != expected.secondarySchema
            || row.provenance != format::ActorSemanticProvenance::executableStatic
            || row.descriptorEvidenceAddress != expected.descriptorEvidenceAddress
            || row.primaryEvidenceAddress != expected.primaryEvidenceAddress
            || row.secondaryEvidenceAddress != expected.secondaryEvidenceAddress
            || row.flags != (format::kSimulationEventDefinitionExact | absentFlags)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.runtimeSchemas.size(); ++index) {
        const RuntimeSchema& row = snapshot.runtimeSchemas[index];
        const ExtractedRuntimeSchema& expected = extracted.runtimeSchemas[index];
        if (row.handle != expected.handle || row.decodedSize != expected.decodedSize
            || row.definitionHash != expected.definitionHash
            || row.definitionClass != expected.definitionClass
            || row.codecFamilies != expected.codecFamilies
            || row.provenance != format::ActorSemanticProvenance::executableStatic
            || row.fields.first != expected.firstField || row.fields.count != expected.fieldCount
            || !range_inside(row.fields, snapshot.runtimeFields.size())
            || row.evidenceAddress != expected.evidenceAddress || row.flags != expected.flags
            || row.arrayElementCount != expected.arrayElementCount) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.runtimeFields.size(); ++index) {
        const RuntimeField& row = snapshot.runtimeFields[index];
        const ExtractedRuntimeField& expected = extracted.runtimeFields[index];
        if (row.schemaIndex != expected.schemaIndex || row.ordinal != expected.ordinal
            || row.structOffset != expected.structOffset
            || row.alternateOffset != expected.alternateOffset || row.typeCode != expected.typeCode
            || row.nestedHandle != expected.nestedHandle || row.bias != expected.bias
            || row.bits != expected.bits || row.codecParameters != expected.codecParameters
            || row.flags != expected.flags || row.schemaIndex >= snapshot.runtimeSchemas.size()) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.runtimeTypes.size(); ++index) {
        const RuntimeTypeDefinition& row = snapshot.runtimeTypes[index];
        const ExtractedRuntimeTypeDefinition& expected = extracted.runtimeTypes[index];
        Text expectedName{};
        if (!text_literal(expected.name, expectedName) || !same_text(row.name, expectedName)
            || row.codecFamilies != expected.codecFamily || row.typeCode != expected.typeCode
            || row.decodedSize != expected.decodedSize || row.fixedBits != expected.fixedBits
            || row.minimumBits != expected.minimumBits || row.maximumBits != expected.maximumBits
            || row.writerEvidenceAddress != expected.writerEvidenceAddress
            || row.readerEvidenceAddress != expected.readerEvidenceAddress
            || row.flags != expected.flags) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.entityTypes.size(); ++index) {
        const EntityTypeDefinition& row = snapshot.entityTypes[index];
        const ExtractedEntityTypeDefinition& expected = extracted.entityTypes[index];
        Text expectedName{};
        if (!text_literal(expected.name, expectedName) || !same_text(row.name, expectedName)
            || row.entityType != expected.entityType
            || row.baselineSchema != expected.baselineSchema
            || row.updateSchema != expected.updateSchema
            || row.provenance != format::ActorSemanticProvenance::executableStatic
            || row.vtableEvidenceAddress != expected.vtableEvidenceAddress
            || row.baselineEvidenceAddress != expected.baselineEvidenceAddress
            || row.updateEvidenceAddress != expected.updateEvidenceAddress
            || row.flags != expected.flags) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.actorStateNames.size(); ++index) {
        const ActorStateName& row = snapshot.actorStateNames[index];
        const bool ordered =
            index == 0 || snapshot.actorStateNames[index - 1U].actorClassIndex < row.actorClassIndex
            || (snapshot.actorStateNames[index - 1U].actorClassIndex == row.actorClassIndex
                && snapshot.actorStateNames[index - 1U].ordinal + 1U == row.ordinal);
        const bool first =
            index == 0
            || snapshot.actorStateNames[index - 1U].actorClassIndex != row.actorClassIndex;
        if (!ordered || (first && row.ordinal != 0)
            || row.actorClassIndex >= snapshot.actorClasses.size()
            || is_absent_tag(row.definitionTag)
            || row.groupHash != format::kActorStateMachineGroupHash || row.nameHash == 0
            || row.nameHash == kAbsentTag || row.flags != format::kActorStateNameExact) {
            return false;
        }
    }
    if (snapshot.sobjectRsatFieldBindings.size() != snapshot.fields.size()) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.sobjectRsatFieldBindings.size(); ++index) {
        const SobjectRsatFieldBinding& binding = snapshot.sobjectRsatFieldBindings[index];
        const RsatField& field = snapshot.fields[index];
        std::uint32_t handle = 0;
        std::uint32_t parameter14 = 0;
        std::uint32_t parameter18 = 0;
        std::uint64_t decodedOffset = 0;
        std::memcpy(&handle, field.rawRow.data() + 0x10U, sizeof handle);
        std::memcpy(&parameter14, field.rawRow.data() + 0x14U, sizeof parameter14);
        std::memcpy(&parameter18, field.rawRow.data() + 0x18U, sizeof parameter18);
        std::memcpy(&decodedOffset, field.rawRow.data() + 0x20U, sizeof decodedOffset);
        const std::uint32_t expectedFlags =
            format::kSobjectRsatFieldBindingExact
            | (handle != format::kAbsentIndex ? format::kSobjectRsatFieldBindingHasRuntimeSchema
                                              : 0U);
        const RuntimeSchema* runtime = nullptr;
        if (handle != format::kAbsentIndex) {
            const auto found =
                std::find_if(snapshot.runtimeSchemas.begin(),
                             snapshot.runtimeSchemas.end(),
                             [handle](const RuntimeSchema& row) { return row.handle == handle; });
            if (found == snapshot.runtimeSchemas.end()) {
                return false;
            }
            runtime = &*found;
        }
        if (binding.rsatFieldIndex != index || binding.runtimeSchemaHandle != handle
            || binding.parameter14 != parameter14 || binding.parameter18 != parameter18
            || binding.decodedOffset != decodedOffset
            || (runtime != nullptr
                && (binding.definitionClass != runtime->definitionClass
                    || binding.codecFamilies != runtime->codecFamilies))
            || (runtime == nullptr && (binding.definitionClass != 0 || binding.codecFamilies != 0))
            || binding.provenance != format::ActorSemanticProvenance::packageField
            || binding.flags != expectedFlags) {
            return false;
        }
    }

    std::vector<std::uint32_t> schemaReferences{};
    std::size_t descriptorCursor = 0;
    try {
        schemaReferences.resize(snapshot.schemas.size());
    } catch (...) {
        return false;
    }
    for (std::size_t actorIndex = 0; actorIndex < snapshot.actorClasses.size(); ++actorIndex) {
        const ActorClass& actor = snapshot.actorClasses[actorIndex];
        const ActorBehaviorProfile& profile = snapshot.behaviorProfiles[actorIndex];
        Text expectedId{};
        const bool absentRsat = is_absent_tag(actor.rsatTag);
        const bool nullArray = !absentRsat && actor.descriptorArrayRelative == 0;
        const bool typedArray =
            !absentRsat
            && stored_typed_array(actor.descriptorArrayOffset,
                                  actor.descriptorArrayRelative,
                                  actor.descriptorArrayHeaderOffset,
                                  actor.descriptorArrayDataOffset)
            && actor.descriptorElementClass == format::kActorRsatDescriptorClass;
        if (!format_actor_id(actor.definitionTag, expectedId) || !same_text(actor.id, expectedId)
            || is_absent_tag(actor.definitionTag)
            || (actorIndex != 0
                && snapshot.actorClasses[actorIndex - 1U].definitionTag >= actor.definitionTag)
            || actor.objectType > 0xFFU || !valid_authored_spawn_profile(actor)
            || profile.actorClassIndex != actorIndex
            || (is_absent_tag(profile.behaviorConfigTag)
                && (profile.behaviorConfigClass != format::kAbsentIndex
                    || profile.behaviorProvenance != format::ActorSemanticProvenance::notPresent))
            || (!is_absent_tag(profile.behaviorConfigTag)
                && (profile.behaviorConfigClass != format::kActorBehaviorConfigClass
                    || profile.behaviorProvenance != format::ActorSemanticProvenance::packageField))
            || profile.behaviorConfigOffset != kActorBehaviorConfigOffset
            || profile.defaultFaction != 0
            || profile.factionProvenance != format::ActorSemanticProvenance::engineZeroDefault
            || profile.flags != format::kActorBehaviorProfileExact
            || actor.descriptors.first != descriptorCursor
            || !range_inside(actor.descriptors, snapshot.descriptors.size())
            || actor.dynamicPresenceTailCount > actor.descriptors.count
            || (absentRsat
                && (actor.rsatReverseDefinitionTag != format::kAbsentIndex
                    || actor.descriptorArrayOffset != format::kAbsentIndex
                    || actor.descriptorArrayRelative != format::kAbsentRelativeOffset
                    || actor.descriptorArrayHeaderOffset != format::kAbsentIndex
                    || actor.descriptorArrayDataOffset != format::kAbsentIndex
                    || actor.descriptorElementClass != format::kAbsentIndex
                    || actor.descriptors.count != 0 || actor.dynamicPresenceTailCount != 0))
            || (!absentRsat
                && (actor.rsatReverseDefinitionTag != actor.definitionTag
                    || actor.descriptorArrayOffset != format::kActorRsatDescriptorArrayOffset
                    || (!nullArray && !typedArray)
                    || (nullArray
                        && (actor.descriptorArrayHeaderOffset != format::kAbsentIndex
                            || actor.descriptorArrayDataOffset != format::kAbsentIndex
                            || actor.descriptorElementClass != format::kAbsentIndex
                            || actor.descriptors.count != 0
                            || actor.dynamicPresenceTailCount != 0))))) {
            return false;
        }
        if (!absentRsat) {
            for (std::size_t priorActorIndex = 0; priorActorIndex < actorIndex; ++priorActorIndex) {
                if (snapshot.actorClasses[priorActorIndex].rsatTag == actor.rsatTag) {
                    return false;
                }
            }
        }

        std::uint32_t tailOrdinal = 0;
        for (std::uint32_t ordinal = 0; ordinal < actor.descriptors.count; ++ordinal) {
            const RsatDescriptor& descriptor = snapshot.descriptors[descriptorCursor + ordinal];
            Text expectedDescriptorId{};
            const bool eligible =
                (descriptor.flags & format::kRsatDescriptorDynamicPresenceEligible) != 0;
            const std::uint64_t offset =
                static_cast<std::uint64_t>(actor.descriptorArrayDataOffset)
                + static_cast<std::uint64_t>(ordinal) * format::kRsatDescriptorRawRowSize;
            std::uint32_t rawComponent = 0;
            std::uint32_t rawSchema = 0;
            std::memcpy(&rawComponent, descriptor.rawRow.data(), sizeof rawComponent);
            std::memcpy(&rawSchema, descriptor.rawRow.data() + 4U, sizeof rawSchema);
            if (!format_descriptor_id(actor.rsatTag, ordinal, expectedDescriptorId)
                || !same_text(descriptor.id, expectedDescriptorId)
                || descriptor.actorClassIndex != actorIndex || descriptor.rsatTag != actor.rsatTag
                || descriptor.descriptorOrdinal != ordinal
                || offset > (std::numeric_limits<std::uint32_t>::max)()
                || descriptor.descriptorOffset != static_cast<std::uint32_t>(offset)
                || descriptor.descriptorElementClass != format::kActorRsatDescriptorClass
                || descriptor.schemaIndex >= snapshot.schemas.size()
                || (descriptor.flags & ~format::kRsatDescriptorFlagMask) != 0
                || descriptor.componentTag != rawComponent || descriptor.schemaTag != rawSchema
                || (eligible && descriptor.dynamicPresenceTailOrdinal != tailOrdinal)
                || (!eligible && descriptor.dynamicPresenceTailOrdinal != format::kAbsentIndex)) {
                return false;
            }
            const RsatSchema& schema = snapshot.schemas[descriptor.schemaIndex];
            const bool schemaEligible =
                (schema.flags & format::kRsatSchemaDynamicPresenceEligible) != 0;
            if (descriptor.schemaTag != schema.schemaTag
                || descriptor.schemaFieldCount != schema.fieldCount
                || descriptor.schemaFirstFieldRuntimeGate != schema.firstFieldRuntimeGate
                || descriptor.schemaFirstFieldRawU32At10 != schema.firstFieldRawU32At10
                || eligible != schemaEligible) {
                return false;
            }
            ++schemaReferences[descriptor.schemaIndex];
            tailOrdinal += eligible ? 1U : 0U;
        }
        if (tailOrdinal != actor.dynamicPresenceTailCount) {
            return false;
        }
        descriptorCursor += actor.descriptors.count;
    }
    if (descriptorCursor != snapshot.descriptors.size()) {
        return false;
    }

    std::size_t sobjectDescriptorCursor = 0;
    for (std::size_t rsatIndex = 0; rsatIndex < snapshot.sobjectRsats.size(); ++rsatIndex) {
        const SobjectRsat& rsat = snapshot.sobjectRsats[rsatIndex];
        const bool nullArray = rsat.descriptorArrayRelative == 0;
        const bool typedArray = stored_typed_array(rsat.descriptorArrayOffset,
                                                   rsat.descriptorArrayRelative,
                                                   rsat.descriptorArrayHeaderOffset,
                                                   rsat.descriptorArrayDataOffset)
                                && rsat.descriptorElementClass == format::kActorRsatDescriptorClass;
        if (is_absent_tag(rsat.rsatTag)
            || (rsatIndex != 0 && snapshot.sobjectRsats[rsatIndex - 1U].rsatTag >= rsat.rsatTag)
            || is_absent_tag(rsat.reverseDefinitionTag)
            || rsat.descriptorArrayOffset != format::kActorRsatDescriptorArrayOffset
            || (!nullArray && !typedArray)
            || (nullArray
                && (rsat.descriptorArrayHeaderOffset != format::kAbsentIndex
                    || rsat.descriptorArrayDataOffset != format::kAbsentIndex
                    || rsat.descriptorElementClass != format::kAbsentIndex
                    || rsat.descriptors.count != 0 || rsat.dynamicPresenceTailCount != 0))
            || rsat.provenance != format::ActorSemanticProvenance::packageField
            || rsat.descriptors.first != sobjectDescriptorCursor
            || !range_inside(rsat.descriptors, snapshot.sobjectRsatDescriptors.size())
            || rsat.dynamicPresenceTailCount > rsat.descriptors.count
            || rsat.flags != format::kSobjectRsatExact || rsat.reserved != 0) {
            return false;
        }
        std::uint32_t tailOrdinal = 0;
        for (std::uint32_t ordinal = 0; ordinal < rsat.descriptors.count; ++ordinal) {
            const SobjectRsatDescriptor& descriptor =
                snapshot.sobjectRsatDescriptors[sobjectDescriptorCursor + ordinal];
            const std::uint64_t expectedOffset =
                static_cast<std::uint64_t>(rsat.descriptorArrayDataOffset)
                + static_cast<std::uint64_t>(ordinal) * format::kRsatDescriptorRawRowSize;
            std::uint32_t rawComponent = 0;
            std::uint32_t rawSchema = 0;
            std::memcpy(&rawComponent, descriptor.rawRow.data(), sizeof rawComponent);
            std::memcpy(&rawSchema, descriptor.rawRow.data() + 4U, sizeof rawSchema);
            const bool eligible =
                (descriptor.flags & format::kSobjectRsatDescriptorDynamicPresenceEligible) != 0;
            if (descriptor.rsatIndex != rsatIndex || descriptor.descriptorOrdinal != ordinal
                || expectedOffset > (std::numeric_limits<std::uint32_t>::max)()
                || descriptor.descriptorOffset != static_cast<std::uint32_t>(expectedOffset)
                || descriptor.componentTag != rawComponent || descriptor.schemaTag != rawSchema
                || descriptor.schemaIndex >= snapshot.schemas.size()
                || descriptor.schemaTag != snapshot.schemas[descriptor.schemaIndex].schemaTag
                || descriptor.schemaFieldCount
                       != snapshot.schemas[descriptor.schemaIndex].fieldCount
                || descriptor.schemaFirstFieldRuntimeGate
                       != snapshot.schemas[descriptor.schemaIndex].firstFieldRuntimeGate
                || (eligible && descriptor.dynamicPresenceTailOrdinal != tailOrdinal)
                || (!eligible && descriptor.dynamicPresenceTailOrdinal != format::kAbsentIndex)
                || (descriptor.flags & ~format::kSobjectRsatDescriptorDynamicPresenceEligible)
                       != 0) {
                return false;
            }
            ++schemaReferences[descriptor.schemaIndex];
            tailOrdinal += eligible ? 1U : 0U;
        }
        if (tailOrdinal != rsat.dynamicPresenceTailCount) {
            return false;
        }
        sobjectDescriptorCursor += rsat.descriptors.count;
    }
    if (sobjectDescriptorCursor != snapshot.sobjectRsatDescriptors.size()
        || snapshot.sobjectRsats.empty()) {
        return false;
    }

    std::size_t fieldCursor = 0;
    for (std::size_t schemaIndex = 0; schemaIndex < snapshot.schemas.size(); ++schemaIndex) {
        const RsatSchema& schema = snapshot.schemas[schemaIndex];
        Text expectedId{};
        const bool typed = (schema.flags & format::kRsatSchemaTypedFieldArray) != 0;
        const bool eligible = (schema.flags & format::kRsatSchemaDynamicPresenceEligible) != 0;
        const bool typedShape = stored_typed_array(schema.fieldArrayOffset,
                                                   schema.fieldArrayRelative,
                                                   schema.fieldArrayHeaderOffset,
                                                   schema.fieldArrayDataOffset)
                                && schema.fieldElementClass == format::kActorRsatSchemaFieldClass;
        if (!format_schema_id(schema.schemaTag, expectedId) || !same_text(schema.id, expectedId)
            || is_absent_tag(schema.schemaTag)
            || (schemaIndex != 0
                && snapshot.schemas[schemaIndex - 1U].schemaTag >= schema.schemaTag)
            || schema.schemaClass != format::kActorRsatSchemaClass
            || schema.fieldCount != schema.fields.count || schema.fields.first != fieldCursor
            || !range_inside(schema.fields, snapshot.fields.size())
            || schema.fieldArrayOffset != format::kActorRsatSchemaFieldArrayOffset
            || (schema.flags & ~format::kRsatSchemaFlagMask) != 0
            || eligible
                   != (schema.fieldCount != 0
                       && schema.firstFieldRuntimeGate != format::kAbsentIndex)
            || (schema.fieldCount == 0
                && (schema.firstFieldRuntimeGate != format::kAbsentIndex
                    || schema.firstFieldRawU32At10 != format::kAbsentIndex))
            || (!typed
                && (schema.fieldCount != 0 || schema.fieldArrayRelative != 0
                    || schema.fieldArrayHeaderOffset != format::kAbsentIndex
                    || schema.fieldArrayDataOffset != format::kAbsentIndex
                    || schema.fieldElementClass != format::kAbsentIndex))
            || (typed && !typedShape) || schemaReferences[schemaIndex] == 0) {
            return false;
        }
        if (schema.fieldCount != 0) {
            std::uint32_t firstGate = 0;
            std::uint32_t firstRaw = 0;
            const RsatField& first = snapshot.fields[fieldCursor];
            std::memcpy(&firstGate, first.rawRow.data(), sizeof firstGate);
            std::memcpy(&firstRaw, first.rawRow.data() + 0x10U, sizeof firstRaw);
            if (firstGate != schema.firstFieldRuntimeGate
                || firstRaw != schema.firstFieldRawU32At10) {
                return false;
            }
        }
        fieldCursor += schema.fieldCount;
    }
    return fieldCursor == snapshot.fields.size();
}

/** Builds an inventory from an already complete, sorted actor-definition tag set. */
bool build_from_tags_and_rsats(std::span<const std::uint32_t> actorTags,
                               std::span<const std::uint32_t> rsatTags,
                               ReadTag readTag,
                               void* readContext,
                               CancelProbe cancel,
                               void* cancelContext,
                               Snapshot& output) noexcept {
    output = {};
    if (actorTags.empty() || readTag == nullptr || is_cancelled(cancel, cancelContext)) {
        return false;
    }
    for (std::size_t index = 0; index < actorTags.size(); ++index) {
        if (is_absent_tag(actorTags[index])
            || (index != 0 && actorTags[index - 1U] >= actorTags[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < rsatTags.size(); ++index) {
        if (is_absent_tag(rsatTags[index])
            || (index != 0 && rsatTags[index - 1U] >= rsatTags[index])) {
            return false;
        }
    }

    try {
        BuildState state{};
        state.readTag = readTag;
        state.readContext = readContext;
        state.cancel = cancel;
        state.cancelContext = cancelContext;
        state.snapshot.actorClasses.reserve(actorTags.size());
        state.schemaSources.reserve(actorTags.size() + rsatTags.size());
        state.schemaIndexes.reserve(actorTags.size() + rsatTags.size());
        state.rsatTags.reserve(actorTags.size());
        if (!add_engine_semantics(state.snapshot)) {
            return false;
        }

        for (std::size_t actorIndex = 0; actorIndex < actorTags.size(); ++actorIndex) {
            if (is_cancelled(cancel, cancelContext)
                || actorIndex > (std::numeric_limits<std::uint32_t>::max)()
                || state.snapshot.descriptors.size()
                       > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            const std::uint32_t actorTag = actorTags[actorIndex];
            std::vector<std::byte> actorBlob{};
            if (!readTag(readContext, actorTag, kActorClassDefinitionClass, actorBlob)
                || actorBlob.size() < kActorDefinitionMinimumSize) {
                return false;
            }

            ActorClass actor{};
            actor.definitionTag = actorTag;
            actor.descriptors.first = static_cast<std::uint32_t>(state.snapshot.descriptors.size());
            std::uint8_t objectType = 0;
            std::uint32_t behaviorConfigTag = format::kAbsentIndex;
            if (!format_actor_id(actorTag, actor.id)
                || !read_value(actorBlob, kActorNameHashOffset, actor.nameHash)
                || !read_value(
                    actorBlob, kActorAuthoredSpawnProfileOffset, actor.authoredSpawnProfile)
                || !read_value(actorBlob, kActorRsatTagOffset, actor.rsatTag)
                || !read_value(actorBlob, kActorObjectTypeOffset, objectType)) {
                return false;
            }
            if (contains(actorBlob, kActorBehaviorConfigOffset, sizeof behaviorConfigTag)
                && !read_value(actorBlob, kActorBehaviorConfigOffset, behaviorConfigTag)) {
                return false;
            }
            actor.objectType = objectType;
            ActorBehaviorProfile profile{};
            profile.actorClassIndex = static_cast<std::uint32_t>(actorIndex);
            profile.behaviorConfigTag = behaviorConfigTag;
            profile.behaviorConfigOffset = kActorBehaviorConfigOffset;
            profile.defaultFaction = 0;
            profile.flags = format::kActorBehaviorProfileExact;
            if (is_absent_tag(behaviorConfigTag)) {
                profile.behaviorConfigClass = format::kAbsentIndex;
                profile.behaviorProvenance = format::ActorSemanticProvenance::notPresent;
            } else {
                std::vector<std::byte> behaviorConfig{};
                if (readTag(readContext,
                            behaviorConfigTag,
                            format::kActorBehaviorConfigClass,
                            behaviorConfig)) {
                    profile.behaviorConfigClass = format::kActorBehaviorConfigClass;
                } else {
                    profile.behaviorConfigTag = format::kAbsentIndex;
                    profile.behaviorConfigClass = format::kAbsentIndex;
                    profile.behaviorProvenance = format::ActorSemanticProvenance::notPresent;
                }
            }
            state.snapshot.behaviorProfiles.push_back(profile);
            if (!collect_state_names(state, static_cast<std::uint32_t>(actorIndex), actorBlob)) {
                return false;
            }
            if (is_absent_tag(actor.rsatTag)) {
                state.snapshot.actorClasses.push_back(std::move(actor));
                continue;
            }
            if (!state.rsatTags.emplace(actor.rsatTag).second) {
                return false;
            }

            std::vector<std::byte> rsatBlob{};
            TypedArray descriptorArray{};
            if (!readTag(readContext, actor.rsatTag, kActorRsatClass, rsatBlob)
                || !read_value(
                    rsatBlob, kActorRsatReverseDefinitionOffset, actor.rsatReverseDefinitionTag)
                || actor.rsatReverseDefinitionTag != actorTag
                || !typed_array(rsatBlob,
                                format::kActorRsatDescriptorArrayOffset,
                                format::kActorRsatDescriptorClass,
                                kDescriptorStride,
                                descriptorArray)) {
                return false;
            }
            actor.descriptorArrayOffset = format::kActorRsatDescriptorArrayOffset;
            actor.descriptorArrayRelative = descriptorArray.relative;
            actor.descriptorArrayHeaderOffset = descriptorArray.headerOffset;
            actor.descriptorArrayDataOffset = descriptorArray.dataOffset;
            actor.descriptorElementClass = descriptorArray.elementClass;
            actor.descriptors.count = descriptorArray.count;
            if (descriptorArray.count
                > (std::numeric_limits<std::uint32_t>::max)() - state.snapshot.descriptors.size()) {
                return false;
            }

            std::uint32_t tailOrdinal = 0;
            for (std::uint32_t ordinal = 0; ordinal < descriptorArray.count; ++ordinal) {
                const std::size_t offset = static_cast<std::size_t>(descriptorArray.dataOffset)
                                           + static_cast<std::size_t>(ordinal) * kDescriptorStride;
                RsatDescriptor descriptor{};
                descriptor.actorClassIndex = static_cast<std::uint32_t>(actorIndex);
                descriptor.rsatTag = actor.rsatTag;
                descriptor.descriptorOrdinal = ordinal;
                descriptor.descriptorElementClass = format::kActorRsatDescriptorClass;
                if (!format_descriptor_id(actor.rsatTag, ordinal, descriptor.id)
                    || !to_u32(offset, descriptor.descriptorOffset)) {
                    return false;
                }
                std::copy_n(
                    rsatBlob.data() + offset, descriptor.rawRow.size(), descriptor.rawRow.begin());
                std::memcpy(&descriptor.componentTag,
                            descriptor.rawRow.data(),
                            sizeof descriptor.componentTag);
                std::memcpy(&descriptor.schemaTag,
                            descriptor.rawRow.data() + 4U,
                            sizeof descriptor.schemaTag);
                std::size_t schemaSourceIndex = 0;
                if (!schema(state, descriptor.schemaTag, schemaSourceIndex)) {
                    return false;
                }
                const RsatSchema& schemaRow = state.schemaSources[schemaSourceIndex].row;
                descriptor.schemaFieldCount = schemaRow.fieldCount;
                descriptor.schemaFirstFieldRuntimeGate = schemaRow.firstFieldRuntimeGate;
                descriptor.schemaFirstFieldRawU32At10 = schemaRow.firstFieldRawU32At10;
                if ((schemaRow.flags & format::kRsatSchemaDynamicPresenceEligible) != 0) {
                    descriptor.flags |= format::kRsatDescriptorDynamicPresenceEligible;
                    descriptor.dynamicPresenceTailOrdinal = tailOrdinal++;
                }
                state.snapshot.descriptors.push_back(std::move(descriptor));
            }
            actor.dynamicPresenceTailCount = tailOrdinal;
            state.snapshot.actorClasses.push_back(std::move(actor));
        }

        std::vector<std::uint32_t> installedRsats(rsatTags.begin(), rsatTags.end());
        installedRsats.reserve(installedRsats.size() + state.rsatTags.size());
        installedRsats.insert(installedRsats.end(), state.rsatTags.begin(), state.rsatTags.end());
        std::sort(installedRsats.begin(), installedRsats.end());
        installedRsats.erase(std::unique(installedRsats.begin(), installedRsats.end()),
                             installedRsats.end());
        state.snapshot.sobjectRsats.reserve(installedRsats.size());
        for (const std::uint32_t rsatTag : installedRsats) {
            if (!sobject_rsat(state, rsatTag)) {
                return false;
            }
        }

        std::sort(state.schemaSources.begin(),
                  state.schemaSources.end(),
                  [](const SchemaSource& left, const SchemaSource& right) {
                      return left.row.schemaTag < right.row.schemaTag;
                  });
        state.schemaIndexes.clear();
        state.schemaIndexes.reserve(state.schemaSources.size());
        state.snapshot.schemas.reserve(state.schemaSources.size());
        for (std::size_t index = 0; index < state.schemaSources.size(); ++index) {
            SchemaSource& source = state.schemaSources[index];
            if (index > (std::numeric_limits<std::uint32_t>::max)()
                || state.snapshot.fields.size() > (std::numeric_limits<std::uint32_t>::max)()
                || source.fields.size() > (std::numeric_limits<std::uint32_t>::max)()
                || source.fields.size()
                       > (std::numeric_limits<std::uint32_t>::max)() - state.snapshot.fields.size()
                || !state.schemaIndexes.emplace(source.row.schemaTag, index).second) {
                return false;
            }
            source.row.fields.first = static_cast<std::uint32_t>(state.snapshot.fields.size());
            source.row.fields.count = static_cast<std::uint32_t>(source.fields.size());
            state.snapshot.schemas.push_back(source.row);
            state.snapshot.fields.insert(
                state.snapshot.fields.end(), source.fields.begin(), source.fields.end());
        }
        state.snapshot.sobjectRsatFieldBindings.reserve(state.snapshot.fields.size());
        for (std::size_t index = 0; index < state.snapshot.fields.size(); ++index) {
            if (index > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            const RsatField& field = state.snapshot.fields[index];
            SobjectRsatFieldBinding binding{};
            binding.rsatFieldIndex = static_cast<std::uint32_t>(index);
            std::memcpy(&binding.runtimeSchemaHandle,
                        field.rawRow.data() + 0x10U,
                        sizeof binding.runtimeSchemaHandle);
            std::memcpy(
                &binding.parameter14, field.rawRow.data() + 0x14U, sizeof binding.parameter14);
            std::memcpy(
                &binding.parameter18, field.rawRow.data() + 0x18U, sizeof binding.parameter18);
            std::memcpy(
                &binding.decodedOffset, field.rawRow.data() + 0x20U, sizeof binding.decodedOffset);
            binding.flags = format::kSobjectRsatFieldBindingExact;
            if (binding.runtimeSchemaHandle != format::kAbsentIndex) {
                const auto runtime =
                    std::find_if(state.snapshot.runtimeSchemas.begin(),
                                 state.snapshot.runtimeSchemas.end(),
                                 [&binding](const RuntimeSchema& row) {
                                     return row.handle == binding.runtimeSchemaHandle;
                                 });
                if (runtime == state.snapshot.runtimeSchemas.end()) {
                    return false;
                }
                binding.definitionClass = runtime->definitionClass;
                binding.codecFamilies = runtime->codecFamilies;
                binding.flags |= format::kSobjectRsatFieldBindingHasRuntimeSchema;
            }
            state.snapshot.sobjectRsatFieldBindings.push_back(binding);
        }
        for (RsatDescriptor& descriptor : state.snapshot.descriptors) {
            const auto found = state.schemaIndexes.find(descriptor.schemaTag);
            if (found == state.schemaIndexes.end()
                || found->second > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            descriptor.schemaIndex = static_cast<std::uint32_t>(found->second);
        }
        for (SobjectRsatDescriptor& descriptor : state.snapshot.sobjectRsatDescriptors) {
            const auto found = state.schemaIndexes.find(descriptor.schemaTag);
            if (found == state.schemaIndexes.end()
                || found->second > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            descriptor.schemaIndex = static_cast<std::uint32_t>(found->second);
        }

        state.snapshot.complete = true;
        if (!validate(state.snapshot)) {
            return false;
        }
        output = std::move(state.snapshot);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool build_from_tags(std::span<const std::uint32_t> actorTags,
                     ReadTag readTag,
                     void* readContext,
                     CancelProbe cancel,
                     void* cancelContext,
                     Snapshot& output) noexcept {
    return build_from_tags_and_rsats(
        actorTags, {}, readTag, readContext, cancel, cancelContext, output);
}

/** Builds the complete current-build inventory for an upstream exact actor tag set. */
bool build(const reader::Source& source,
           std::span<const std::uint32_t> actorTags,
           CancelProbe cancel,
           void* cancelContext,
           Snapshot& output) noexcept {
    return build_with_rsats(source, actorTags, {}, cancel, cancelContext, output);
}

/** Builds the actor inventory from tags plus already-read RSAT rows. */
bool build_with_rsats(const reader::Source& source,
                      std::span<const std::uint32_t> actorTags,
                      std::span<const std::uint32_t> rsatTags,
                      CancelProbe cancel,
                      void* cancelContext,
                      Snapshot& output) noexcept {
    output = {};
    if (source.directory.empty() || source.keys == nullptr || actorTags.empty()
        || is_cancelled(cancel, cancelContext)) {
        return false;
    }
    try {
        ScratchOwner scratch{};
        if (scratch.get() == nullptr) {
            return false;
        }
        ParallelReadOwner parallelReads{};
        PackageReadContext context{&source, scratch.get()};
        std::vector<reader::parallel::Held> held{};
        if (!reader::parallel::read_kept(source, rsatTags, held)) {
            return false;
        }
        std::vector<std::uint32_t> schemaTags{};
        context.prefetched.reserve(held.size());
        for (const reader::parallel::Held& row : held) {
            std::uint32_t classId = 0;
            TypedArray descriptors{};
            if (!reader::read_tag_class(source, *scratch.get(), row.tag, classId)
                || classId != kActorRsatClass
                || !typed_array(row.blob,
                                format::kActorRsatDescriptorArrayOffset,
                                format::kActorRsatDescriptorClass,
                                kDescriptorStride,
                                descriptors)) {
                return false;
            }
            for (std::uint32_t ordinal = 0; ordinal < descriptors.count; ++ordinal) {
                const std::size_t offset = static_cast<std::size_t>(descriptors.dataOffset)
                                           + static_cast<std::size_t>(ordinal) * kDescriptorStride;
                std::uint32_t schemaTag = 0;
                if (!read_value(row.blob, offset + 4U, schemaTag) || is_absent_tag(schemaTag)) {
                    return false;
                }
                schemaTags.push_back(schemaTag);
            }
            PrefetchedTag cached{};
            cached.bytes.assign(row.blob.begin(), row.blob.end());
            cached.classId = kActorRsatClass;
            if (!context.prefetched.emplace(row.tag, std::move(cached)).second) {
                return false;
            }
        }
        std::sort(schemaTags.begin(), schemaTags.end());
        schemaTags.erase(std::unique(schemaTags.begin(), schemaTags.end()), schemaTags.end());
        if (is_cancelled(cancel, cancelContext)
            || !reader::parallel::read_kept(source, schemaTags, held)) {
            return false;
        }
        context.prefetched.reserve(context.prefetched.size() + held.size());
        for (const reader::parallel::Held& row : held) {
            std::uint32_t classId = 0;
            if (!reader::read_tag_class(source, *scratch.get(), row.tag, classId)
                || classId != format::kActorRsatSchemaClass) {
                return false;
            }
            PrefetchedTag cached{};
            cached.bytes.assign(row.blob.begin(), row.blob.end());
            cached.classId = format::kActorRsatSchemaClass;
            if (!context.prefetched.emplace(row.tag, std::move(cached)).second) {
                return false;
            }
        }
        return build_from_tags_and_rsats(
            actorTags, rsatTags, &package_read, &context, cancel, cancelContext, output);
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory
