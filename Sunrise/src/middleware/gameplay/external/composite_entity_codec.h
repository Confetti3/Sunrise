#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "actor_command_runtime_codec.h"
#include "external_entity_codec.h"
#include "sobject_payload_codec.h"

namespace sunrise::middleware::gameplay::external {

/** One server process retains at most this many live channel-2 session mirrors. */
inline constexpr std::size_t kCompositeEntitySessionCapacity = 16;

/** One committed channel-2 identity used to resolve update-only records. */
struct EntityBaselineSlot final {
    std::uint32_t rsatTag{};
    std::uint8_t allocationSequence{};
    std::uint8_t incarnation{};
    EntityType type{EntityType::sobject};
    bool occupied{};
};

/** Caller-owned state is scoped to one peer session and simulation view. */
struct EntityBaselineRegistry final {
    state::activity_sdk::Snapshot catalog{};
    std::array<EntityBaselineSlot, kMaximumEntitySlot + 1U> slots{};
};

/** One compare-and-commit registry change staged from an accepted batch. */
struct EntityBaselineMutation final {
    EntityBaselineSlot expected{};
    EntityBaselineSlot replacement{};
    std::uint16_t slot{};
    bool valid{};
};

/** Stable caller-owned dependencies for the composite payload callbacks. */
struct CompositeEntityCodecContext final {
    state::activity_sdk::Snapshot catalog{};
    std::span<const state::activity_sdk::format::EntityTypeDefinition> entityTypes{};
    std::span<const state::activity_sdk::format::SobjectRsat> sobjectRsats{};
    std::span<const state::activity_sdk::format::SobjectRsatDescriptor> sobjectDescriptors{};
    std::span<const state::activity_sdk::format::RsatSchema> rsatSchemas{};
    std::span<const state::activity_sdk::format::RsatField> rsatFields{};
    std::span<const state::activity_sdk::format::SobjectRsatFieldBinding> sobjectBindings{};
    std::span<const state::activity_sdk::format::RuntimeSchema> runtimeSchemas{};
    std::span<const state::activity_sdk::format::RuntimeField> runtimeFields{};
    std::span<const state::activity_sdk::format::RuntimeTypeDefinition> runtimeTypes{};
    EntityBaselineRegistry* registry{};
    SobjectPositionCompression positionCompression{SobjectPositionCompression::disabled};
    bool ready{};
};

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Synthetic row views are accepted only by focused test binaries. */
struct CompositeEntityCatalogFixture final {
    std::span<const state::activity_sdk::format::EntityTypeDefinition> entityTypes{};
    std::span<const state::activity_sdk::format::SobjectRsat> sobjectRsats{};
    std::span<const state::activity_sdk::format::SobjectRsatDescriptor> sobjectDescriptors{};
    std::span<const state::activity_sdk::format::RsatSchema> rsatSchemas{};
    std::span<const state::activity_sdk::format::RsatField> rsatFields{};
    std::span<const state::activity_sdk::format::SobjectRsatFieldBinding> sobjectBindings{};
    std::span<const state::activity_sdk::format::RuntimeSchema> runtimeSchemas{};
    std::span<const state::activity_sdk::format::RuntimeField> runtimeFields{};
    std::span<const state::activity_sdk::format::RuntimeTypeDefinition> runtimeTypes{};
};

/** Builds a codec over deterministic in-memory v32 rows without publishing a pack. */
[[nodiscard]] bool
initialize_composite_entity_codec_for_test(CompositeEntityCodecContext& context,
                                           EntityBaselineRegistry& registry,
                                           const CompositeEntityCatalogFixture& fixture,
                                           SobjectPositionCompression positionCompression) noexcept;
#endif

/** One session binds its own token baseline space to the shared immutable catalog. */
struct CompositeEntitySession final {
    EntityBaselineRegistry registry{};
    CompositeEntityCodecContext codec{};
    std::uint64_t groupSessionId{};
    bool occupied{};
};

/** Bounded mirror state for all channel-2 sessions owned by one gameplay runtime. */
struct CompositeEntitySessionStore final {
    state::activity_sdk::Snapshot catalog{};
    std::array<CompositeEntitySession, kCompositeEntitySessionCapacity> sessions{};
    SobjectPositionCompression positionCompression{SobjectPositionCompression::disabled};
};

/** Pins and validates the current v32 entity reflection catalog. */
[[nodiscard]] bool
initialize_composite_entity_codec(CompositeEntityCodecContext& context,
                                  EntityBaselineRegistry& registry,
                                  SobjectPositionCompression positionCompression) noexcept;

/** Binds one already authenticated SDK snapshot without changing global publication. */
[[nodiscard]] bool initialize_composite_entity_codec_from_catalog(
    CompositeEntityCodecContext& context,
    EntityBaselineRegistry& registry,
    const state::activity_sdk::Snapshot& catalog,
    SobjectPositionCompression positionCompression) noexcept;

/** Builds a mirror-only type payload codec over stable caller-owned context. */
[[nodiscard]] TypePayloadCodec
make_composite_entity_payload_codec(const CompositeEntityCodecContext& context) noexcept;

/** Decodes one retained typed mirror through its published executable schema. */
[[nodiscard]] bool decode_composite_entity_payload(
    const state::activity_sdk::Snapshot& catalog,
    EntityType type,
    TypePayloadPart part,
    const TypePayload& payload,
    std::span<middleware::bap::activity_message::wire_schema::RuntimeDecodedValue> values,
    middleware::bap::activity_message::wire_schema::RuntimeDecodeResult& result) noexcept;

/** Validates one decoded batch and stages its single registry change. */
[[nodiscard]] bool stage_entity_baseline_mutation(const CompositeEntityCodecContext& context,
                                                  const EntityBatch& batch,
                                                  EntityBaselineMutation& output) noexcept;

/** Commits a staged change only when its source registry state is unchanged. */
[[nodiscard]] bool commit_accepted_entity_batch(EntityBaselineRegistry& registry,
                                                const EntityBaselineMutation& mutation) noexcept;

/** Pins the shared catalog used by every later session in this store. */
[[nodiscard]] bool
initialize_composite_entity_sessions(CompositeEntitySessionStore& store,
                                     SobjectPositionCompression positionCompression) noexcept;

/** Reads one batch against the exact session's committed update-only baselines. */
[[nodiscard]] bool read_composite_entity_batch(CompositeEntitySessionStore& store,
                                               std::uint64_t groupSessionId,
                                               middleware::encoding::bits::Reader& reader,
                                               EntityBatch& output) noexcept;

/** Peer accepted-record adapter. It stages and commits only for the named session. */
[[nodiscard]] bool accept_composite_entity_batch(const void* context,
                                                 std::uint64_t groupSessionId,
                                                 const EntityBatch& batch) noexcept;

/** Removes all token baselines owned by one ended group session. */
void reset_composite_entity_session(CompositeEntitySessionStore& store,
                                    std::uint64_t groupSessionId) noexcept;

} // namespace sunrise::middleware::gameplay::external
