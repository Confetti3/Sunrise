#include "composite_entity_codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>

#include "../../encoding/bit_raw.h"

namespace sunrise::middleware::gameplay::external {
namespace {

namespace format = state::activity_sdk::format;
namespace wire = actor_wire;
namespace bits = middleware::encoding::bits;

/** Marks a retained mirror body so another callback's layout cannot be read as one. */
constexpr std::uint32_t kMirrorIdentity = 0x4345324DU;
/** The transform's packed angle code is seven bits. */
constexpr std::uint8_t kAngleWidth = 7;
/** Position components travel as raw 32-bit floats. */
constexpr std::uint8_t kFloatWidth = 32;
/** A transform names X, Y and Z. The fourth homogeneous lane is implied. */
constexpr std::size_t kPositionComponents = 3;
/** The walker view stores the type code and codec parameter 2 as one byte each. */
constexpr std::uint32_t kMaximumByteField = 0xFFU;
/** A schema reference the mirror retains must fit one 32-bit tag. */
constexpr std::uint64_t kMaximumSemanticTag = 0xFFFFFFFFULL;

/** Private header distinguishes retained mirror bodies from other payload layouts. */
struct MirrorHeader final {
    std::uint32_t identity{kMirrorIdentity};
    std::uint32_t semanticTag{};
    std::uint16_t bitCount{};
    std::uint16_t reserved{};
};

/** One validated header and its exact body bytes. */
struct MirrorView final {
    MirrorHeader header{};
    std::span<const std::byte> bytes{};
};

/** One reflection walk is bound to an SDK view and codec family. */
struct ResolverContext final {
    const CompositeEntityCodecContext* catalog{};
    format::RuntimeCodecFamily family{format::RuntimeCodecFamily::activity};
};

/** Finds one unique runtime schema handle in the pinned view. */
[[nodiscard]] const format::RuntimeSchema*
runtime_schema(const CompositeEntityCodecContext& context, std::uint32_t handle) noexcept {
    for (const format::RuntimeSchema& row : context.runtimeSchemas) {
        if (row.handle == handle) {
            return &row;
        }
    }
    return nullptr;
}

/** Finds one installed RSAT in its generated tag order. */
[[nodiscard]] const format::SobjectRsat* sobject_rsat(const CompositeEntityCodecContext& context,
                                                      std::uint32_t tag) noexcept {
    const auto found =
        std::lower_bound(context.sobjectRsats.begin(),
                         context.sobjectRsats.end(),
                         tag,
                         [](const auto& row, auto value) { return row.rsatTag < value; });
    return found != context.sobjectRsats.end() && found->rsatTag == tag ? &*found : nullptr;
}

/** Builds one bounded canonical body while the input is decoded. */
class MirrorBuilder final {
public:
    /** Opens an empty fixed-capacity bit writer. */
    MirrorBuilder() noexcept : writer_(bytes_) {}

    /** Appends one scalar field. */
    [[nodiscard]] bool append(std::uint64_t value, std::uint8_t width) noexcept {
        return writer_.write(value, width);
    }

    /** Appends an exact meaningful-bit prefix from byte storage. */
    [[nodiscard]] bool append(std::span<const std::byte> bytes, std::size_t bitCount) noexcept {
        bits::Reader reader(bytes);
        return bits::copy(reader, writer_, bitCount);
    }

    /** Seals the canonical body into callback-owned payload state. */
    [[nodiscard]] bool finish(std::uint32_t semanticTag, TypePayload& output) noexcept {
        std::size_t byteCount = 0;
        if (!writer_.finish(byteCount) || writer_.bit_count() > kMaximumTypePayloadBits
            || byteCount + sizeof(MirrorHeader) > output.state.size()) {
            return false;
        }
        MirrorHeader header{};
        header.semanticTag = semanticTag;
        header.bitCount = static_cast<std::uint16_t>(writer_.bit_count());
        TypePayload candidate{};
        std::memcpy(candidate.state.data(), &header, sizeof(header));
        std::memcpy(candidate.state.data() + sizeof(header), bytes_.data(), byteCount);
        candidate.byteCount = static_cast<std::uint16_t>(sizeof(header) + byteCount);
        output = candidate;
        return true;
    }

private:
    std::array<std::byte, kMaximumTypePayloadBits / 8U> bytes_{};
    bits::Writer writer_;
};

/** Compares every field protected by a staged registry commit. */
[[nodiscard]] bool same_slot(const EntityBaselineSlot& left,
                             const EntityBaselineSlot& right) noexcept {
    return left.rsatTag == right.rsatTag && left.allocationSequence == right.allocationSequence
           && left.incarnation == right.incarnation && left.type == right.type
           && left.occupied == right.occupied;
}

/** Checks the complete 17-bit entity-token domain. */
[[nodiscard]] bool valid_token(const EntityToken& token) noexcept {
    return token.slot <= kMaximumEntitySlot && token.incarnation <= kMaximumEntityIncarnation;
}

/** Validates and opens one callback-owned canonical mirror. */
[[nodiscard]] bool load_mirror(const TypePayload& payload, MirrorView& output) noexcept {
    if (payload.byteCount < sizeof(MirrorHeader) || payload.byteCount > payload.state.size()) {
        return false;
    }
    MirrorHeader header{};
    std::memcpy(&header, payload.state.data(), sizeof(header));
    const std::size_t byteCount = (static_cast<std::size_t>(header.bitCount) + 7U) / 8U;
    if (header.identity != kMirrorIdentity || header.reserved != 0
        || header.bitCount > kMaximumTypePayloadBits
        || payload.byteCount != sizeof(header) + byteCount) {
        return false;
    }
    output.header = header;
    output.bytes = {payload.state.data() + sizeof(header), byteCount};
    return true;
}

/** Reads one field and retains the same wire bits. */
[[nodiscard]] bool read_and_append(bits::Reader& reader,
                                   MirrorBuilder& mirror,
                                   std::uint8_t width,
                                   std::uint64_t& output) noexcept {
    return reader.read(width, output) && mirror.append(output, width);
}

/** Reads and retains one required boolean field. */
[[nodiscard]] bool read_flag(bits::Reader& reader, MirrorBuilder& mirror, bool& output) noexcept {
    std::uint64_t value = 0;
    if (!read_and_append(reader, mirror, kFlagWidth, value)) {
        return false;
    }
    output = value != 0;
    return true;
}

/** Resolves one schema only when it supports the selected codec family. */
[[nodiscard]] bool
find_schema(const void* raw, std::uint32_t handle, wire::runtime::SchemaView& output) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr) {
        return false;
    }
    const format::RuntimeSchema* row = runtime_schema(*context->catalog, handle);
    constexpr std::uint32_t kAllowedFlags =
        format::kRuntimeSchemaExact | format::kRuntimeSchemaArrayRegion;
    const bool arrayRegion =
        row != nullptr && (row->flags & format::kRuntimeSchemaArrayRegion) != 0;
    if (row == nullptr || (row->flags & format::kRuntimeSchemaExact) == 0
        || (row->flags & ~kAllowedFlags) != 0 || arrayRegion != (row->arrayElementCount != 0)
        || (row->codecFamilies & static_cast<std::uint32_t>(context->family)) == 0) {
        return false;
    }
    const auto schemas = context->catalog->runtimeSchemas;
    const auto fields = context->catalog->runtimeFields;
    if (row < schemas.data() || row >= schemas.data() + schemas.size()
        || row->fields.first > fields.size()
        || row->fields.count > fields.size() - row->fields.first) {
        return false;
    }
    output.row = static_cast<std::uint32_t>(row - schemas.data());
    output.handle = row->handle;
    output.arrayLength = row->arrayElementCount;
    output.firstField = row->fields.first;
    output.fieldCount = row->fields.count;
    output.structSize = row->decodedSize;
    return true;
}

/** Resolves one schema by its stable generated row index. */
[[nodiscard]] bool
read_schema(const void* raw, std::uint32_t rowIndex, wire::runtime::SchemaView& output) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr
        || rowIndex >= context->catalog->runtimeSchemas.size()) {
        return false;
    }
    return find_schema(raw, context->catalog->runtimeSchemas[rowIndex].handle, output)
           && output.row == rowIndex;
}

/** Converts one SDK field row into the generic runtime walker view. */
[[nodiscard]] bool
read_field(const void* raw, std::uint32_t rowIndex, wire::runtime::FieldView& output) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr
        || rowIndex >= context->catalog->runtimeFields.size()) {
        return false;
    }
    const format::RuntimeField& row = context->catalog->runtimeFields[rowIndex];
    if (row.schemaIndex >= context->catalog->runtimeSchemas.size()
        || (row.flags & format::kRuntimeFieldExact) == 0 || row.typeCode > kMaximumByteField) {
        return false;
    }
    std::uint32_t nestedRow = wire::runtime::kAbsentRuntimeRow;
    if (row.nestedHandle != format::kAbsentIndex) {
        wire::runtime::SchemaView nested{};
        if (!find_schema(raw, row.nestedHandle, nested)) {
            return false;
        }
        nestedRow = nested.row;
    }
    std::int32_t bias = 0;
    if (row.bias != format::kAbsentSignedValue) {
        if (row.bias < (std::numeric_limits<std::int32_t>::min)()
            || row.bias > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        bias = static_cast<std::int32_t>(row.bias);
    }
    const bool dynamicArray = (row.flags & format::kRuntimeFieldDynamicArray) != 0;
    const bool quantized =
        row.typeCode == static_cast<std::uint32_t>(format::RuntimeFieldType::quantizedFloat);
    output = {};
    output.row = rowIndex;
    output.nestedSchemaRow = nestedRow;
    output.structOffset = row.structOffset;
    output.biasOrDynamic = quantized      ? static_cast<std::int32_t>(row.codecParameters[0])
                           : dynamicArray ? 1
                                          : bias;
    output.widthOrCountOffset =
        quantized || dynamicArray          ? static_cast<std::int32_t>(row.codecParameters[1])
        : row.bits == format::kAbsentIndex ? 0
                                           : static_cast<std::int32_t>(row.bits);
    output.typeCode = static_cast<std::uint8_t>(row.typeCode);
    output.presence =
        static_cast<std::uint8_t>((row.flags & format::kRuntimeFieldPresenceBit) != 0);
    output.parameter2 = static_cast<std::uint8_t>(
        row.codecParameters[2] <= kMaximumByteField ? row.codecParameters[2] : 0);
    return true;
}

/** Accepts a union selector only when its family-specific SDK row exists. */
[[nodiscard]] bool validate_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr) {
        return false;
    }
    const std::uint32_t family = static_cast<std::uint32_t>(context->family);
    return std::any_of(context->catalog->runtimeTypes.begin(),
                       context->catalog->runtimeTypes.end(),
                       [family, typeCode](const format::RuntimeTypeDefinition& row) {
                           return row.typeCode == typeCode && (row.codecFamilies & family) != 0;
                       });
}

/** Accepts one exact family-specific unsupported type only when it consumes no bits. */
[[nodiscard]] bool zero_bit_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr) {
        return false;
    }
    const std::uint32_t family = static_cast<std::uint32_t>(context->family);
    const format::RuntimeTypeDefinition* match = nullptr;
    for (const format::RuntimeTypeDefinition& row : context->catalog->runtimeTypes) {
        if (row.typeCode != typeCode || (row.codecFamilies & family) == 0) {
            continue;
        }
        if (match != nullptr) {
            return false;
        }
        match = &row;
    }
    return match != nullptr && (match->flags & format::kRuntimeTypeDefinitionExact) != 0
           && (match->flags & format::kRuntimeTypeUnsupported) != 0 && match->fixedBits == 0
           && match->minimumBits == 0 && match->maximumBits == 0 && match->reserved == 0;
}

/** Builds borrowed reflection callbacks over one stable resolver context. */
[[nodiscard]] wire::RuntimeSchemaResolver make_resolver(ResolverContext& context) noexcept {
    wire::RuntimeSchemaResolver resolver{};
    resolver.context = &context;
    resolver.findSchema = find_schema;
    resolver.readSchema = read_schema;
    resolver.readField = read_field;
    resolver.validateType = validate_type;
    resolver.isZeroBitType = zero_bit_type;
    return resolver;
}

/** Decodes, re-encodes, and retains one complete reflected schema body. */
[[nodiscard]] bool append_schema(bits::Reader& reader,
                                 MirrorBuilder& mirror,
                                 ResolverContext& resolverContext,
                                 std::uint32_t schemaHandle,
                                 std::uint32_t* semanticTag) noexcept {
    const wire::RuntimeSchemaResolver resolver = make_resolver(resolverContext);
    std::array<wire::RuntimeDecodedValue, wire::kRuntimeValueCapacity> values{};
    wire::RuntimeDecodeResult result{};
    if (!wire::decode_full_schema_prefix(schemaHandle, reader, resolver, values, result)
        || result.status != wire::CodecStatus::complete || result.valuesTruncated
        || result.valueCount > values.size()) {
        return false;
    }
    std::array<wire::RuntimeDraftValue, wire::kRuntimeValueCapacity> draft{};
    for (std::size_t index = 0; index < result.valueCount; ++index) {
        static_cast<wire::RuntimeDraftValue&>(draft[index]) = values[index];
    }
    std::array<std::byte, kMaximumTypePayloadBits / 8U> encoded{};
    std::size_t written = 0;
    std::size_t writtenBits = 0;
    wire::CodecStatus status = wire::CodecStatus::malformed;
    if (!wire::encode_full_schema(
            schemaHandle,
            std::span<const wire::RuntimeDraftValue>{draft.data(), result.valueCount},
            resolver,
            encoded,
            written,
            writtenBits,
            status)
        || status != wire::CodecStatus::complete || writtenBits != result.bitsConsumed
        || !mirror.append(std::span<const std::byte>{encoded.data(), written}, writtenBits)) {
        return false;
    }
    if (semanticTag != nullptr) {
        const std::uint64_t nullableType =
            static_cast<std::uint32_t>(format::RuntimeFieldType::nullableTag);
        bool selected = false;
        bool found = false;
        std::uint32_t tag = 0;
        for (std::size_t index = 0; index < result.valueCount; ++index) {
            const wire::RuntimeDecodedValue& value = values[index];
            if (value.role == wire::ValueRole::variantSelector
                && value.unsignedValue == nullableType) {
                selected = true;
            } else if (selected && value.role == wire::ValueRole::schemaReference && value.present
                       && value.unsignedValue <= kMaximumSemanticTag) {
                if (found) {
                    return false;
                }
                found = true;
                tag = static_cast<std::uint32_t>(value.unsignedValue);
            }
        }
        if (!found || tag == 0 || tag == format::kAbsentIndex) {
            return false;
        }
        *semanticTag = tag;
    }
    return true;
}

/** Resolves one exact channel-2 type contract from the SDK. */
[[nodiscard]] const format::EntityTypeDefinition*
entity_definition(const CompositeEntityCodecContext& context, EntityType type) noexcept {
    for (const format::EntityTypeDefinition& row : context.entityTypes) {
        if (row.entityType == static_cast<std::uint32_t>(type)) {
            return (row.flags & format::kEntityTypeDefinitionExact) != 0 ? &row : nullptr;
        }
    }
    return nullptr;
}

/** Decodes one retained typed mirror through its published executable schema. */
bool decode_composite_entity_payload_impl(const state::activity_sdk::Snapshot& catalog,
                                          EntityType type,
                                          TypePayloadPart part,
                                          const TypePayload& payload,
                                          std::span<wire::RuntimeDecodedValue> values,
                                          wire::RuntimeDecodeResult& result) noexcept {
    result = {};
    if (catalog == nullptr) {
        return false;
    }
    CompositeEntityCodecContext context{};
    context.catalog = catalog;
    context.entityTypes = catalog->entity_type_definitions();
    context.sobjectRsats = catalog->sobject_rsats();
    context.sobjectDescriptors = catalog->sobject_rsat_descriptors();
    context.rsatSchemas = catalog->rsat_schemas();
    context.rsatFields = catalog->rsat_fields();
    context.sobjectBindings = catalog->sobject_rsat_field_bindings();
    context.runtimeSchemas = catalog->runtime_schemas();
    context.runtimeFields = catalog->runtime_fields();
    context.runtimeTypes = catalog->runtime_type_definitions();
    context.ready = true;
    const format::EntityTypeDefinition* definition = entity_definition(context, type);
    const std::uint32_t schemaHandle = definition == nullptr ? format::kAbsentIndex
                                       : part == TypePayloadPart::baseline
                                           ? definition->baselineSchema
                                           : definition->updateSchema;
    MirrorView mirror{};
    if (schemaHandle == format::kAbsentIndex || !load_mirror(payload, mirror)) {
        return false;
    }
    ResolverContext resolverContext{&context, format::RuntimeCodecFamily::activity};
    const wire::RuntimeSchemaResolver resolver = make_resolver(resolverContext);
    return wire::decode_full_schema(
               schemaHandle, mirror.bytes, mirror.header.bitCount, resolver, values, result)
           && result.status == wire::CodecStatus::complete && !result.valuesTruncated;
}

/** Maps the selected position mode to its matching SObject codec family. */
[[nodiscard]] format::RuntimeCodecFamily
sobject_family(SobjectPositionCompression compression) noexcept {
    return compression == SobjectPositionCompression::disabled
               ? format::RuntimeCodecFamily::sobjectModeZero
               : format::RuntimeCodecFamily::sobjectModeOne;
}

/** Reads the supported exact type-0 transform prefix. */
[[nodiscard]] bool append_sobject_prefix(bits::Reader& reader,
                                         MirrorBuilder& mirror,
                                         SobjectPositionCompression compression) noexcept {
    bool transform = false;
    bool rotationShortcut = false;
    bool ignoredFlag = false;
    std::uint64_t ignored = 0;
    if (!read_flag(reader, mirror, transform) || !transform
        || !read_flag(reader, mirror, rotationShortcut) || !rotationShortcut
        || !read_flag(reader, mirror, ignoredFlag)
        || !read_and_append(reader, mirror, kAngleWidth, ignored)
        || !read_flag(reader, mirror, ignoredFlag) || !ignoredFlag) {
        return false;
    }
    if (compression == SobjectPositionCompression::enabledRaw) {
        bool compressed = false;
        if (!read_flag(reader, mirror, compressed) || compressed) {
            return false;
        }
    } else if (compression != SobjectPositionCompression::disabled) {
        return false;
    }
    for (std::size_t index = 0; index < kPositionComponents; ++index) {
        std::uint64_t raw = 0;
        if (!read_and_append(reader, mirror, kFloatWidth, raw)
            || !std::isfinite(std::bit_cast<float>(static_cast<std::uint32_t>(raw)))) {
            return false;
        }
    }
    bool parent = false;
    bool streamSource = false;
    return read_flag(reader, mirror, parent) && !parent && read_flag(reader, mirror, streamSource)
           && !streamSource;
}

/** Walks every ordered RSAT presence bit and each present component schema. */
[[nodiscard]] bool append_sobject_components(const CompositeEntityCodecContext& context,
                                             std::uint32_t rsatTag,
                                             bits::Reader& reader,
                                             MirrorBuilder& mirror) noexcept {
    const format::SobjectRsat* rsat = sobject_rsat(context, rsatTag);
    if (rsat == nullptr || rsat->flags != format::kSobjectRsatExact) {
        return false;
    }
    if (rsat->descriptors.first > context.sobjectDescriptors.size()
        || rsat->descriptors.count > context.sobjectDescriptors.size() - rsat->descriptors.first) {
        return false;
    }
    const auto descriptors =
        context.sobjectDescriptors.subspan(rsat->descriptors.first, rsat->descriptors.count);
    std::uint32_t tailOrdinal = 0;
    ResolverContext resolver{&context, sobject_family(context.positionCompression)};
    const auto schemas = context.rsatSchemas;
    for (const format::SobjectRsatDescriptor& descriptor : descriptors) {
        const bool eligible =
            (descriptor.flags & format::kSobjectRsatDescriptorDynamicPresenceEligible) != 0;
        if (!eligible) {
            if (descriptor.dynamicPresenceTailOrdinal != format::kAbsentIndex) {
                return false;
            }
            continue;
        }
        if (descriptor.dynamicPresenceTailOrdinal != tailOrdinal++) {
            return false;
        }
        bool present = false;
        if (!read_flag(reader, mirror, present)) {
            return false;
        }
        if (!present) {
            continue;
        }
        if (descriptor.schemaIndex >= schemas.size()) {
            return false;
        }
        const format::RsatSchema& schema = schemas[descriptor.schemaIndex];
        if (schema.fields.first > context.rsatFields.size()
            || schema.fields.count > context.rsatFields.size() - schema.fields.first) {
            return false;
        }
        const auto fields = context.rsatFields.subspan(schema.fields.first, schema.fields.count);
        if (fields.size() != descriptor.schemaFieldCount) {
            return false;
        }
        for (const format::RsatField& field : fields) {
            const std::size_t fieldIndex =
                static_cast<std::size_t>(&field - context.rsatFields.data());
            const format::SobjectRsatFieldBinding* binding =
                fieldIndex < context.sobjectBindings.size()
                        && context.sobjectBindings[fieldIndex].rsatFieldIndex == fieldIndex
                    ? &context.sobjectBindings[fieldIndex]
                    : nullptr;
            const std::uint32_t family = static_cast<std::uint32_t>(resolver.family);
            if (binding == nullptr
                || (binding->flags
                    & (format::kSobjectRsatFieldBindingExact
                       | format::kSobjectRsatFieldBindingHasRuntimeSchema))
                       != (format::kSobjectRsatFieldBindingExact
                           | format::kSobjectRsatFieldBindingHasRuntimeSchema)
                || (binding->codecFamilies & family) == 0
                || !append_schema(
                    reader, mirror, resolver, binding->runtimeSchemaHandle, nullptr)) {
                return false;
            }
        }
    }
    return tailOrdinal == rsat->dynamicPresenceTailCount;
}

/** Resolves one update-only type-0 RSAT from committed session state. */
[[nodiscard]] bool registry_sobject_tag(const CompositeEntityCodecContext& context,
                                        const EntityToken& token,
                                        std::uint32_t& output) noexcept {
    if (context.registry == nullptr || !valid_token(token)) {
        return false;
    }
    const EntityBaselineSlot& slot = context.registry->slots[token.slot];
    if (!slot.occupied || slot.incarnation != token.incarnation || slot.type != EntityType::sobject
        || slot.rsatTag == 0) {
        return false;
    }
    output = slot.rsatTag;
    return true;
}

/** Dispatches one baseline or update through its SDK-selected grammar. */
[[nodiscard]] bool read_payload_impl(const CompositeEntityCodecContext& context,
                                     const EntityToken& token,
                                     EntityType type,
                                     TypePayloadPart part,
                                     const TypePayload* baseline,
                                     bits::Reader& reader,
                                     TypePayload& output) noexcept {
    const format::EntityTypeDefinition* definition = entity_definition(context, type);
    if (definition == nullptr || (definition->flags & format::kEntityTypeStockEmittable) == 0) {
        return false;
    }
    MirrorBuilder mirror{};
    std::uint32_t semanticTag = 0;
    if (part == TypePayloadPart::baseline) {
        if (baseline != nullptr || definition->baselineSchema == format::kAbsentIndex) {
            return false;
        }
        ResolverContext resolver{&context, format::RuntimeCodecFamily::activity};
        if (!append_schema(reader,
                           mirror,
                           resolver,
                           definition->baselineSchema,
                           type == EntityType::sobject ? &semanticTag : nullptr)) {
            return false;
        }
        if (type == EntityType::sobject) {
            bool placementIdentity = false;
            if (!read_flag(reader, mirror, placementIdentity) || !placementIdentity) {
                return false;
            }
        }
        return mirror.finish(semanticTag, output);
    }
    if ((definition->flags & format::kEntityTypeUpdateSupported) == 0) {
        return false;
    }
    if ((definition->flags & format::kEntityTypeUpdateUsesSobjectRsat) != 0) {
        if (type != EntityType::sobject) {
            return false;
        }
        if (baseline != nullptr) {
            MirrorView baselineView{};
            if (!load_mirror(*baseline, baselineView)) {
                return false;
            }
            semanticTag = baselineView.header.semanticTag;
        } else if (!registry_sobject_tag(context, token, semanticTag)) {
            return false;
        }
        return semanticTag != 0
               && append_sobject_prefix(reader, mirror, context.positionCompression)
               && append_sobject_components(context, semanticTag, reader, mirror)
               && mirror.finish(semanticTag, output);
    }
    if (definition->updateSchema == format::kAbsentIndex) {
        return false;
    }
    ResolverContext resolver{&context, format::RuntimeCodecFamily::activity};
    return append_schema(reader, mirror, resolver, definition->updateSchema, nullptr)
           && mirror.finish(0, output);
}

/** TypePayloadCodec reader adapter with no registry mutation. */
[[nodiscard]] bool read_payload(const void* raw,
                                const EntityToken& token,
                                EntityType type,
                                TypePayloadPart part,
                                const TypePayload* baseline,
                                bits::Reader& reader,
                                TypePayload& output) noexcept {
    const auto* const context = static_cast<const CompositeEntityCodecContext*>(raw);
    return context != nullptr && context->ready
           && read_payload_impl(*context, token, type, part, baseline, reader, output);
}

/** Replays exactly the meaningful bits from one validated mirror. */
[[nodiscard]] bool write_mirror(bits::Writer& writer, const MirrorView& mirror) noexcept {
    bits::Reader reader(mirror.bytes);
    return bits::copy(reader, writer, mirror.header.bitCount);
}

/** Revalidates a mirror through reflection before replaying it. */
[[nodiscard]] bool write_payload(const void* raw,
                                 const EntityToken& token,
                                 EntityType type,
                                 TypePayloadPart part,
                                 const TypePayload* baseline,
                                 const TypePayload& payload,
                                 bits::Writer& writer) noexcept {
    const auto* const context = static_cast<const CompositeEntityCodecContext*>(raw);
    MirrorView view{};
    if (context == nullptr || !context->ready || !load_mirror(payload, view)) {
        return false;
    }
    bits::Reader verifier(view.bytes);
    TypePayload canonical{};
    MirrorView canonicalView{};
    if (!read_payload_impl(*context, token, type, part, baseline, verifier, canonical)
        || !load_mirror(canonical, canonicalView) || canonical.byteCount != payload.byteCount
        || std::memcmp(canonical.state.data(), payload.state.data(), payload.byteCount) != 0) {
        return false;
    }
    return write_mirror(writer, view);
}

/** Resolves an update-only entity type from its session registry. */
[[nodiscard]] bool
resolve_type(const void* raw, const EntityToken& token, EntityType& output) noexcept {
    const auto* const context = static_cast<const CompositeEntityCodecContext*>(raw);
    if (context == nullptr || context->registry == nullptr || !valid_token(token)) {
        return false;
    }
    const EntityBaselineSlot& slot = context->registry->slots[token.slot];
    if (!slot.occupied || slot.incarnation != token.incarnation
        || entity_definition(*context, slot.type) == nullptr) {
        return false;
    }
    output = slot.type;
    return true;
}

/** Compares wrapping allocation sequences within the unambiguous half-range. */
[[nodiscard]] bool serial_is_newer(std::uint8_t candidate, std::uint8_t current) noexcept {
    /** Half of the byte sequence space. A larger gap cannot be ordered. */
    constexpr std::uint8_t kOrderableDistance = 0x80U;
    const std::uint8_t distance = static_cast<std::uint8_t>(candidate - current);
    return distance != 0 && distance < kOrderableDistance;
}

/** Validates the complete row closure required by the composite codec. */
[[nodiscard]] bool valid_catalog_rows(const CompositeEntityCodecContext& context) noexcept {
    if (context.entityTypes.size() != static_cast<std::size_t>(EntityType::count)
        || context.sobjectRsats.empty() || context.sobjectDescriptors.empty()
        || context.sobjectBindings.size() != context.rsatFields.size()
        || context.runtimeSchemas.empty() || context.runtimeFields.empty()
        || context.runtimeTypes.empty()
        || (context.positionCompression != SobjectPositionCompression::disabled
            && context.positionCompression != SobjectPositionCompression::enabledRaw)) {
        return false;
    }
    std::array<bool, static_cast<std::size_t>(EntityType::count)> found{};
    for (const format::EntityTypeDefinition& row : context.entityTypes) {
        if (row.entityType >= found.size() || found[row.entityType]
            || (row.flags & format::kEntityTypeDefinitionExact) == 0) {
            return false;
        }
        const bool stock = (row.flags & format::kEntityTypeStockEmittable) != 0;
        const bool update = (row.flags & format::kEntityTypeUpdateSupported) != 0;
        const bool dynamic = (row.flags & format::kEntityTypeUpdateUsesSobjectRsat) != 0;
        if ((stock && row.baselineSchema == format::kAbsentIndex)
            || (dynamic && (!update || row.updateSchema != format::kAbsentIndex))
            || (!dynamic && update && row.updateSchema == format::kAbsentIndex)) {
            return false;
        }
        if (stock && runtime_schema(context, row.baselineSchema) == nullptr) {
            return false;
        }
        if (!dynamic && update && runtime_schema(context, row.updateSchema) == nullptr) {
            return false;
        }
        found[row.entityType] = true;
    }
    return std::all_of(found.begin(), found.end(), [](bool value) { return value; });
}

/** Pins one authenticated SDK snapshot and resets stale registry ownership. */
[[nodiscard]] bool bind_context(CompositeEntityCodecContext& context,
                                EntityBaselineRegistry& registry,
                                const state::activity_sdk::Snapshot& catalog,
                                SobjectPositionCompression positionCompression) noexcept {
    if (catalog == nullptr
        || (positionCompression != SobjectPositionCompression::disabled
            && positionCompression != SobjectPositionCompression::enabledRaw)) {
        return false;
    }
    if (registry.catalog != catalog) {
        registry.catalog = catalog;
        registry.slots.fill({});
    }
    context.catalog = catalog;
    context.entityTypes = catalog->entity_type_definitions();
    context.sobjectRsats = catalog->sobject_rsats();
    context.sobjectDescriptors = catalog->sobject_rsat_descriptors();
    context.rsatSchemas = catalog->rsat_schemas();
    context.rsatFields = catalog->rsat_fields();
    context.sobjectBindings = catalog->sobject_rsat_field_bindings();
    context.runtimeSchemas = catalog->runtime_schemas();
    context.runtimeFields = catalog->runtime_fields();
    context.runtimeTypes = catalog->runtime_type_definitions();
    context.registry = &registry;
    context.positionCompression = positionCompression;
    context.ready = valid_catalog_rows(context);
    return context.ready;
}

/** Finds or creates one bounded session without evicting another session. */
[[nodiscard]] CompositeEntitySession*
session(CompositeEntitySessionStore& store, std::uint64_t groupSessionId, bool create) noexcept {
    if (groupSessionId == 0 || store.catalog == nullptr) {
        return nullptr;
    }
    CompositeEntitySession* empty = nullptr;
    for (CompositeEntitySession& candidate : store.sessions) {
        if (candidate.occupied && candidate.groupSessionId == groupSessionId) {
            return &candidate;
        }
        if (!candidate.occupied && empty == nullptr) {
            empty = &candidate;
        }
    }
    if (!create || empty == nullptr
        || !bind_context(empty->codec, empty->registry, store.catalog, store.positionCompression)) {
        return nullptr;
    }
    empty->groupSessionId = groupSessionId;
    empty->occupied = true;
    return empty;
}

} // namespace

/** Decodes one retained typed mirror through its published executable schema. */
bool decode_composite_entity_payload(const state::activity_sdk::Snapshot& catalog,
                                     EntityType type,
                                     TypePayloadPart part,
                                     const TypePayload& payload,
                                     std::span<wire::RuntimeDecodedValue> values,
                                     wire::RuntimeDecodeResult& result) noexcept {
    return decode_composite_entity_payload_impl(catalog, type, part, payload, values, result);
}

/** Binds the currently published authenticated SDK. */
bool initialize_composite_entity_codec(CompositeEntityCodecContext& context,
                                       EntityBaselineRegistry& registry,
                                       SobjectPositionCompression positionCompression) noexcept {
    state::activity_sdk::Snapshot catalog = state::activity_sdk::snapshot();
    return bind_context(context, registry, catalog, positionCompression);
}

/** Binds one caller-pinned authenticated SDK. */
bool initialize_composite_entity_codec_from_catalog(
    CompositeEntityCodecContext& context,
    EntityBaselineRegistry& registry,
    const state::activity_sdk::Snapshot& catalog,
    SobjectPositionCompression positionCompression) noexcept {
    return bind_context(context, registry, catalog, positionCompression);
}

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Binds deterministic row spans in focused test binaries only. */
bool initialize_composite_entity_codec_for_test(
    CompositeEntityCodecContext& context,
    EntityBaselineRegistry& registry,
    const CompositeEntityCatalogFixture& fixture,
    SobjectPositionCompression positionCompression) noexcept {
    CompositeEntityCodecContext candidate{};
    candidate.entityTypes = fixture.entityTypes;
    candidate.sobjectRsats = fixture.sobjectRsats;
    candidate.sobjectDescriptors = fixture.sobjectDescriptors;
    candidate.rsatSchemas = fixture.rsatSchemas;
    candidate.rsatFields = fixture.rsatFields;
    candidate.sobjectBindings = fixture.sobjectBindings;
    candidate.runtimeSchemas = fixture.runtimeSchemas;
    candidate.runtimeFields = fixture.runtimeFields;
    candidate.runtimeTypes = fixture.runtimeTypes;
    candidate.registry = &registry;
    candidate.positionCompression = positionCompression;
    candidate.ready = valid_catalog_rows(candidate);
    if (!candidate.ready) {
        return false;
    }
    registry = {};
    context = candidate;
    return true;
}
#endif

/** Builds pure callbacks over one stable codec context. */
TypePayloadCodec
make_composite_entity_payload_codec(const CompositeEntityCodecContext& context) noexcept {
    TypePayloadCodec codec{};
    codec.context = &context;
    codec.resolveType = resolve_type;
    codec.read = read_payload;
    codec.write = write_payload;
    codec.maximumBaselineBits = kMaximumTypePayloadBits;
    codec.maximumUpdateBits = kMaximumTypePayloadBits;
    return codec;
}

/** Validates one record and captures its compare-and-commit state. */
bool stage_entity_baseline_mutation(const CompositeEntityCodecContext& context,
                                    const EntityBatch& batch,
                                    EntityBaselineMutation& output) noexcept {
    output = {};
    if (!context.ready || context.registry == nullptr
        || (context.catalog != nullptr && context.registry->catalog != context.catalog)
        || !batch.recordPresent || !valid_token(batch.record.token)) {
        return false;
    }
    const EntityRecord& record = batch.record;
    const EntityBaselineSlot current = context.registry->slots[record.token.slot];
    EntityBaselineMutation candidate{};
    candidate.expected = current;
    candidate.replacement = current;
    candidate.slot = record.token.slot;

    if (record.flags == entityRemove) {
        if (!current.occupied || current.incarnation != record.token.incarnation) {
            return false;
        }
        candidate.replacement = {};
    } else if ((record.flags & entityCreate) != 0) {
        const format::EntityTypeDefinition* definition = entity_definition(context, record.type);
        if (definition == nullptr || (definition->flags & format::kEntityTypeStockEmittable) == 0) {
            return false;
        }
        if (current.occupied) {
            const bool duplicate = current.incarnation == record.token.incarnation
                                   && current.allocationSequence == record.allocationSequence
                                   && current.type == record.type;
            if (!duplicate
                && !serial_is_newer(record.allocationSequence, current.allocationSequence)) {
                return false;
            }
        }
        std::uint32_t rsatTag = 0;
        if (record.type == EntityType::sobject) {
            MirrorView baseline{};
            if (!load_mirror(record.baseline, baseline) || baseline.header.semanticTag == 0
                || sobject_rsat(context, baseline.header.semanticTag) == nullptr) {
                return false;
            }
            rsatTag = baseline.header.semanticTag;
        }
        candidate.replacement = {
            rsatTag, record.allocationSequence, record.token.incarnation, record.type, true};
    } else {
        if (!current.occupied || current.incarnation != record.token.incarnation) {
            return false;
        }
        const format::EntityTypeDefinition* definition = entity_definition(context, current.type);
        if (definition == nullptr
            || (definition->flags & format::kEntityTypeUpdateSupported) == 0) {
            return false;
        }
    }
    candidate.valid = true;
    output = candidate;
    return true;
}

/** Applies a staged mutation only when its source slot has not changed. */
bool commit_accepted_entity_batch(EntityBaselineRegistry& registry,
                                  const EntityBaselineMutation& mutation) noexcept {
    if (!mutation.valid || mutation.slot > kMaximumEntitySlot
        || !same_slot(registry.slots[mutation.slot], mutation.expected)) {
        return false;
    }
    registry.slots[mutation.slot] = mutation.replacement;
    return true;
}

/** Pins one catalog for a new bounded session store. */
bool initialize_composite_entity_sessions(CompositeEntitySessionStore& store,
                                          SobjectPositionCompression positionCompression) noexcept {
    state::activity_sdk::Snapshot catalog = state::activity_sdk::snapshot();
    CompositeEntitySession probe{};
    if (!bind_context(probe.codec, probe.registry, catalog, positionCompression)) {
        return false;
    }
    store = {};
    store.catalog = std::move(catalog);
    store.positionCompression = positionCompression;
    return true;
}

/** Decodes one batch against the named session's committed baselines. */
bool read_composite_entity_batch(CompositeEntitySessionStore& store,
                                 std::uint64_t groupSessionId,
                                 bits::Reader& reader,
                                 EntityBatch& output) noexcept {
    CompositeEntitySession* selected = session(store, groupSessionId, true);
    if (selected == nullptr) {
        return false;
    }
    const TypePayloadCodec codec = make_composite_entity_payload_codec(selected->codec);
    return read_entity_batch(reader, codec, output);
}

/** Stages and commits one peer-accepted batch in its named session. */
bool accept_composite_entity_batch(const void* raw,
                                   std::uint64_t groupSessionId,
                                   const EntityBatch& batch) noexcept {
    auto* const store = const_cast<CompositeEntitySessionStore*>(
        static_cast<const CompositeEntitySessionStore*>(raw));
    if (store == nullptr) {
        return false;
    }
    CompositeEntitySession* selected = session(*store, groupSessionId, false);
    EntityBaselineMutation mutation{};
    return selected != nullptr && stage_entity_baseline_mutation(selected->codec, batch, mutation)
           && commit_accepted_entity_batch(selected->registry, mutation);
}

/** Releases every baseline owned by one ended session. */
void reset_composite_entity_session(CompositeEntitySessionStore& store,
                                    std::uint64_t groupSessionId) noexcept {
    CompositeEntitySession* selected = session(store, groupSessionId, false);
    if (selected != nullptr) {
        *selected = {};
    }
}

} // namespace sunrise::middleware::gameplay::external
