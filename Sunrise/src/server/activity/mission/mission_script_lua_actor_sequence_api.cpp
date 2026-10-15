#include <array>
#include <charconv>
#include <limits>

#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

// Lua sequence handles use these private metatable identities.
constexpr const char* kSequencesMetatable = "SunriseActorSequences";
constexpr const char* kSequenceMetatable = "SunriseActorSequence";

struct SequenceHandle final {
    ActorSequenceOwner owner{};
    std::uint32_t ordinal{};
};

/** An actor value cannot cross a slot or SDK/client generation boundary. */
[[nodiscard]] bool current_owner(lua_State* state, const ActorSequenceOwner& owner) {
    const Impl* const impl = impl_from_state(state);
    ActorSequenceOwner current{};
    return impl != nullptr && impl->definitions.actorSequences.owner != nullptr
           && impl->definitions.actorSequences.owner(
               impl->definitions.actorSequences.context, owner.slotRow, current)
           && current == owner;
}

/** Rechecks ownership before resolving the same authored ordinal. */
[[nodiscard]] bool
current_sequence(lua_State* state, const SequenceHandle& handle, ActorSequenceDefinition& output) {
    const Impl* const impl = impl_from_state(state);
    return current_owner(state, handle.owner) && impl->definitions.actorSequences.resolve != nullptr
           && impl->definitions.actorSequences.resolve(
               impl->definitions.actorSequences.context, handle.owner, handle.ordinal, output);
}

using SequenceBinding = Binding<ActorSequenceDefinition>;

/** Actor sequence members in table order. */
constexpr std::array<SequenceBinding, std::size(api::kActorSequenceMembers)> kSequenceBindings{{
    {"id",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         push_optional_name(state, row.id);
     }},
    {"name",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         push_optional_name(state, row.name);
     }},
    {"symbol",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         push_optional_name(state, row.symbol);
     }},
    {"source_path",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         push_optional_name(state, row.sourcePath);
     }},
    {"key",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         lua_pushinteger(state, row.keyHash);
     }},
    {"kind",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         lua_pushinteger(state, row.kind);
     }},
    {"resource_tag",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         lua_pushinteger(state, row.resourceTag);
     }},
    {"table_index",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         lua_pushinteger(state, row.tableIndex);
     }},
    {"ordinal",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         lua_pushinteger(state, row.ordinal);
     }},
    {"source_offset",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         lua_pushinteger(state, row.sourceOffset);
     }},
    {"playable",
     nullptr,
     [](lua_State* state, const ActorSequenceDefinition& row) {
         lua_pushboolean(state, row.playable);
     }},
}};
static_assert(bound_to<ActorSequenceDefinition>(api::kActorSequenceMembers, kSequenceBindings));

/** Exposes all extracted kinds without making unsupported entries executable. */
[[nodiscard]] int sequence_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SequenceHandle*>(luaL_checkudata(state, 1, kSequenceMetatable));
    ActorSequenceDefinition row{};
    if (!current_sequence(state, *handle, row)) {
        return luaL_error(state, "actor sequence owner is stale");
    }
    return dispatch<ActorSequenceDefinition>(
        state, kSequenceBindings, luaL_checkstring(state, 2), row);
}

/** Collection rows are one-based; an absent ordinal returns nil. */
[[nodiscard]] int sequences_at(lua_State* state) {
    const auto* const owner =
        static_cast<const ActorSequenceOwner*>(luaL_checkudata(state, 1, kSequencesMetatable));
    if (!current_owner(state, *owner)) {
        return luaL_error(state, "actor sequence owner is stale");
    }
    const lua_Integer ordinal = luaL_checkinteger(state, 2);
    ActorSequenceDefinition row{};
    const SequenceHandle handle{*owner, static_cast<std::uint32_t>(ordinal)};
    if (ordinal <= 0 || ordinal > (std::numeric_limits<std::uint32_t>::max)()
        || !current_sequence(state, handle, row)) {
        lua_pushnil(state);
    } else {
        push_handle(state, kSequenceMetatable, handle);
    }
    return 1;
}

/** What a sequence collection read resolves before its key. */
struct SequencesSubject final {
    std::size_t count{};
};

/** Sequence collection members in table order; symbols are served after them. */
constexpr std::array<Binding<SequencesSubject>, std::size(api::kActorSequencesMembers)>
    kSequencesBindings{{
        {"count",
         nullptr,
         [](lua_State* state, const SequencesSubject& subject) {
             lua_pushinteger(state, static_cast<lua_Integer>(subject.count));
         }},
        {"at", &sequences_at},
    }};
static_assert(bound_to<SequencesSubject>(api::kActorSequencesMembers, kSequencesBindings));

/** Named members are the extractor's collision-safe symbols. */
[[nodiscard]] int sequences_index(lua_State* state) {
    const auto* const owner =
        static_cast<const ActorSequenceOwner*>(luaL_checkudata(state, 1, kSequencesMetatable));
    if (!current_owner(state, *owner)) {
        return luaL_error(state, "actor sequence owner is stale");
    }
    const auto& sequences = impl_from_state(state)->definitions.actorSequences;
    const std::size_t count =
        sequences.count != nullptr ? sequences.count(sequences.context, *owner) : 0;
    const std::string_view key = luaL_checkstring(state, 2);
    if (!serve<SequencesSubject>(state, kSequencesBindings, key, SequencesSubject{count})) {
        std::uint32_t keyHash = 0;
        bool alias = false;
        if (key.size() == 12 && key.starts_with("KEY_")) {
            const auto parsed =
                std::from_chars(key.data() + 4, key.data() + key.size(), keyHash, 16);
            alias = parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size();
        }
        for (std::size_t ordinal = 1; ordinal <= count; ++ordinal) {
            ActorSequenceDefinition row{};
            if (!sequences.resolve(
                    sequences.context, *owner, static_cast<std::uint32_t>(ordinal), row)) {
                continue;
            }
            if (alias ? row.keyHash == keyHash : row.symbol == key) {
                push_handle(state,
                            kSequenceMetatable,
                            SequenceHandle{*owner, static_cast<std::uint32_t>(ordinal)});
                return 1;
            }
        }
        lua_pushnil(state);
    }
    return 1;
}

// The table entry of the actor form of play_sequence.
constexpr const api::MemberSpec& kPlayActorSequence =
    api::slot_member("play_sequence", api::slot_auth::kType2SlotType);

} // namespace

/** Binds the catalog to the selected combatant before any sequence handle is minted. */
int slot_actor_sequences(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    ActorSequenceOwner owner{};
    const Impl* const impl = impl_from_state(state);
    if (!current_slot(state, *handle, slot) || !exact_combatant_slot(slot)
        || impl->definitions.actorSequences.owner == nullptr
        || impl->definitions.actorSequences.count == nullptr
        || impl->definitions.actorSequences.resolve == nullptr
        || !impl->definitions.actorSequences.owner(
            impl->definitions.actorSequences.context, slot.nativeRow, owner)) {
        return luaL_error(state, "combatant has no exact authored actor owner");
    }
    push_handle(state, kSequencesMetatable, owner);
    return 1;
}

/** Only an extracted value owned by this exact combatant can become a playback intent. */
int slot_play_actor_sequence(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_combatant_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact combatant");
    }
    refuse_unknown_arguments(state, kPlayActorSequence);
    static_cast<void>(
        push_argument(state, required_key(kPlayActorSequence, "sequence", api::ValueKind::handle)));
    const SequenceHandle sequence =
        *static_cast<const SequenceHandle*>(luaL_checkudata(state, -1, kSequenceMetatable));
    lua_pop(state, 1);
    ActorSequenceDefinition row{};
    if (sequence.owner.slotRow != slot.nativeRow || !current_sequence(state, sequence, row)) {
        return luaL_error(state, "sequence belongs to another combatant or generation");
    }
    if (!row.playable) {
        return luaL_error(state, "extracted sequence kind is not playable");
    }
    Intent intent{};
    intent.kind = IntentKind::playActorSequence;
    intent.firstRow = slot.nativeRow;
    intent.secondRow = row.catalogRow;
    intent.sequenceOwner = sequence.owner;
    return queue_intent(state, active_frame(state), intent);
}

void register_actor_sequence_metatables(lua_State* state) {
    register_metatable(state, kSequencesMetatable, &sequences_index);
    register_metatable(state, kSequenceMetatable, &sequence_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
