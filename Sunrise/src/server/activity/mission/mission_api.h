#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "mission_api_context.h"
#include "mission_api_events.h"
#include "mission_api_handles.h"
#include "mission_api_slot.h"
#include "mission_api_types.h"
#include "mission_api_values.h"

// The mission Lua API table: every receiver and its members, every event, the program keys and
// the closed collections. The VM dispatches from it and the SDK generator emits it.

namespace sunrise::server::activity::mission::api {

/** Every receiver, indexed by Receiver. */
inline constexpr std::array<ReceiverSpec, 34> kReceivers{{
    {.id = Receiver::context,
     .name = "context",
     .luaClass = "SunriseContext",
     .members = kContextMembers,
     .source = "first callback argument"},
    {.id = Receiver::modConfig,
     .name = "mod_config",
     .luaClass = "SunriseModConfig",
     .dynamicKeys =
         "each key the mod declares in mod.json reads its current integer, number or boolean; "
         "any other key reads nil",
     .source = "context.mod_config"},
    {.id = Receiver::state,
     .name = "state",
     .luaClass = "SunriseStateView",
     .members = kStateMembers,
     .source = "second callback argument"},
    {.id = Receiver::requestKey,
     .name = "request_key",
     .luaClass = "SunriseRequestKey",
     .members = kRequestKeyMembers,
     .source = "returned by every request"},
    {.id = Receiver::timers,
     .name = "timers",
     .luaClass = "SunriseTimerNames",
     .members = kTimersMembers,
     .source = "context.timers"},
    {.id = Receiver::timerRef,
     .name = "timer_ref",
     .luaClass = "SunriseTimerName",
     .members = kNameRefMembers,
     .source = "context.timers:resolve(name)"},
    {.id = Receiver::variables,
     .name = "variables",
     .luaClass = "SunriseVariableNames",
     .members = kVariablesMembers,
     .source = "context.variables"},
    {.id = Receiver::variableRef,
     .name = "variable_ref",
     .luaClass = "SunriseVariableName",
     .members = kNameRefMembers,
     .source = "context.variables:resolve(name)"},
    {.id = Receiver::slot,
     .name = "slot",
     .luaClass = "SunriseSlotHandle",
     .members = kSlotMembers,
     .source = "context:slot(selector), event.slot, context.sdk.slots:at(n)"},
    {.id = Receiver::squad,
     .name = "squad",
     .luaClass = "SunriseSquadHandle",
     .members = kSquadMembers,
     .source = "context:squad(selector), context.sdk.squads:at(n)"},
    {.id = Receiver::countVector,
     .name = "count_vector",
     .luaClass = "SunriseSquadCounts",
     .members = kCountVectorMembers,
     .source = "squad:counts()"},
    {.id = Receiver::scene,
     .name = "scene",
     .luaClass = "SunriseSceneHandle",
     .members = kSceneMembers,
     .source = "context:scene(selector), context.sdk.authored_scenes:at(n)"},
    {.id = Receiver::cohort,
     .name = "cohort",
     .luaClass = "SunriseCohort",
     .members = kCohortMembers,
     .source = "context:cohort{squads = list}"},
    {.id = Receiver::lifetime,
     .name = "lifetime",
     .luaClass = "SunriseLifetime",
     .members = kLifetimeMembers,
     .source = "context.lifetime"},
    {.id = Receiver::peers,
     .name = "peers",
     .luaClass = "SunrisePeers",
     .members = kPeersMembers,
     .source = "context.peers"},
    {.id = Receiver::peer,
     .name = "peer",
     .luaClass = "SunrisePeer",
     .members = kPeerMembers,
     .source = "context.peers:at(i)"},
    {.id = Receiver::sdk,
     .name = "sdk",
     .luaClass = "SunriseActivityView",
     .members = kSdkMembers,
     .dynamicKeys =
         "identity, binding fields, row collections and the catalog, world and manifest views; "
         "see the live SDK views page",
     .complete = false,
     .source = "context.sdk"},
    {.id = Receiver::deviceChannels,
     .name = "device_channels",
     .luaClass = "SunriseDeviceChannels",
     .members = kDeviceChannelsMembers,
     .entries = "device_channels",
     .source = "context.sdk.device_channels"},
    {.id = Receiver::deviceChannel,
     .name = "device_channel",
     .luaClass = "SunriseDeviceChannel",
     .members = kDeviceChannelMembers,
     .source = "context.sdk.device_channels.<name>"},
    {.id = Receiver::deviceTransitions,
     .name = "device_transitions",
     .luaClass = "SunriseDeviceTransitions",
     .members = kDeviceTransitionsMembers,
     .entries = "device_transitions",
     .source = "context.sdk.device_transitions"},
    {.id = Receiver::deviceTransition,
     .name = "device_transition",
     .luaClass = "SunriseDeviceTransition",
     .members = kDeviceTransitionMembers,
     .source = "context.sdk.device_transitions.<name>"},
    {.id = Receiver::squadModes,
     .name = "squad_modes",
     .luaClass = "SunriseSquadModes",
     .members = kSquadModesMembers,
     .entries = "squad_modes",
     .source = "context.sdk.squad_modes"},
    {.id = Receiver::squadMode,
     .name = "squad_mode",
     .luaClass = "SunriseSquadMode",
     .members = kSquadModeMembers,
     .source = "context.sdk.squad_modes.<name>"},
    {.id = Receiver::unitScalar,
     .name = "unit_scalar",
     .luaClass = "SunriseUnitScalar",
     .members = kUnitScalarMembers,
     .source = "context.sdk.unit(x)"},
    {.id = Receiver::worldPosition,
     .name = "world_position",
     .luaClass = "SunriseWorldPosition",
     .members = kWorldPositionMembers,
     .source = "context.sdk.position(x, y, z)"},
    {.id = Receiver::lifetimeStates,
     .name = "lifetime_states",
     .luaClass = "SunriseLifetimeStates",
     .members = kLifetimeStatesMembers,
     .source = "context.sdk.lifetime_states"},
    {.id = Receiver::lifetimeState,
     .name = "lifetime_state",
     .luaClass = "SunriseLifetimeState",
     .members = kLifetimeStateMembers,
     .source = "context.sdk.lifetime_states:at(n)"},
    {.id = Receiver::boundedLanes,
     .name = "bounded_lanes",
     .luaClass = "SunriseBoundedLanes",
     .members = kBoundedLanesMembers,
     .entries = "bounded_lanes",
     .source = "context.sdk.bounded_lanes"},
    {.id = Receiver::boundedLane,
     .name = "bounded_lane",
     .luaClass = "SunriseBoundedLane",
     .members = kBoundedLaneMembers,
     .source = "context.sdk.bounded_lanes.<name>"},
    {.id = Receiver::countList,
     .name = "count_list",
     .luaClass = "SunriseCountList",
     .members = kCountListMembers,
     .source = "lane:list() on a count lane"},
    {.id = Receiver::objectRefList,
     .name = "object_ref_list",
     .luaClass = "SunriseObjectRefList",
     .members = kObjectRefListMembers,
     .source = "lane:list() on an object-reference lane"},
    {.id = Receiver::actorSequences,
     .name = "actor_sequences",
     .luaClass = "SunriseActorSequences",
     .members = kActorSequencesMembers,
     .dynamicKeys =
         "a sequence symbol, or KEY_ and the 8 hex digits of its key hash; nil when absent",
     .source = "slot:sequences() on a type-2 actor"},
    {.id = Receiver::actorSequence,
     .name = "actor_sequence",
     .luaClass = "SunriseActorSequence",
     .members = kActorSequenceMembers,
     .source = "a member of the sequence collection"},
    {.id = Receiver::activityMessage,
     .name = "activity_message",
     .luaClass = "SunriseActivityMessage",
     .members = kActivityMessageMembers,
     .dynamicKeys = "the catalog row fields; see the live SDK views page",
     .complete = false,
     .source = "event.message, context.sdk.activity_messages"},
}};

/** @return One receiver's members. */
[[nodiscard]] consteval std::span<const MemberSpec> members(Receiver id) {
    return kReceivers[static_cast<std::size_t>(id)].members;
}

/** @return One member of a receiver. Fails compilation when it is not declared. */
[[nodiscard]] consteval const MemberSpec& member(Receiver id, std::string_view name) {
    return find_member(members(id), name);
}

/** @return One field group by id. */
[[nodiscard]] consteval std::span<const MemberSpec> group_members(FieldGroupId id) {
    return kFieldGroups[static_cast<std::size_t>(id)].members;
}

// --- Table checks ------------------------------------------------------------------------------

/** @return True when every receiver sits at its own index and every member list is well formed. */
[[nodiscard]] consteval bool valid_receivers() {
    for (std::size_t index = 0; index < kReceivers.size(); ++index) {
        const ReceiverSpec& receiver = kReceivers[index];
        if (static_cast<std::size_t>(receiver.id) != index || !valid_members(receiver.members)) {
            return false;
        }
    }
    return true;
}

/**
 * @return True when kinds run in order, callback names are distinct, an event's fields have
 * distinct names, and kinds sharing a Lua class share their groups.
 */
[[nodiscard]] consteval bool valid_events() {
    for (const FieldGroup& entry : kFieldGroups) {
        if (!valid_members(entry.members)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < std::size(kEvents); ++index) {
        const EventSpec& event = kEvents[index];
        if (event.kind != index || !event.callback.starts_with("on_event_")
            || !terminated(event.callback)) {
            return false;
        }
        for (std::size_t other = 0; other < std::size(kEvents); ++other) {
            const EventSpec& peer = kEvents[other];
            if (other != index
                && (peer.callback == event.callback
                    || (peer.luaClass == event.luaClass
                        && peer.groups.data() != event.groups.data()))) {
                return false;
            }
        }
        for (std::size_t first = 0; first < event.groups.size(); ++first) {
            for (std::size_t second = first; second < event.groups.size(); ++second) {
                for (const MemberSpec& left : event.groups[first]->members) {
                    for (const MemberSpec& right : event.groups[second]->members) {
                        if (&left != &right && left.name == right.name) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

/** @return True when every atom kind declares kind and quantized and its fields are well formed. */
[[nodiscard]] consteval bool valid_atoms() {
    for (const AtomKindSpec& kind : kAtomKinds) {
        if (!terminated(kind.name) || !distinct_names(kind.fields)
            || find_field(kind.fields, "kind").kind != ValueKind::string
            || find_field(kind.fields, "quantized").kind != ValueKind::integer) {
            return false;
        }
        for (const ValueSpec& value : kind.fields) {
            if (!valid_value(value)) {
                return false;
            }
        }
    }
    return true;
}

/** @return True when every program key and every field nested in one is well formed. */
[[nodiscard]] consteval bool valid_program() {
    for (const ValueSpec& entry : kProgramEntries) {
        if (!valid_value(entry)) {
            return false;
        }
    }
    return distinct_names(kProgramEntries);
}

/** @return True when every handle in a value, nested ones included, names a known receiver. */
[[nodiscard]] consteval bool known_ref(const ValueSpec& value) {
    if (value.kind == ValueKind::handle) {
        // event and squad_anchors are handles with no receiver row.
        bool known = value.ref == "event" || value.ref == "squad_anchors";
        for (const ReceiverSpec& receiver : kReceivers) {
            known = known || receiver.name == value.ref;
        }
        if (!known) {
            return false;
        }
    }
    for (const ValueSpec& nested : value.element) {
        if (!known_ref(nested)) {
            return false;
        }
    }
    for (const ValueSpec& nested : value.fields) {
        if (!known_ref(nested)) {
            return false;
        }
    }
    return true;
}

/** @return True when every argument and return of a member list passes known_ref. */
[[nodiscard]] consteval bool known_refs(std::span<const MemberSpec> members) {
    for (const MemberSpec& member : members) {
        for (const ValueSpec& value : member.arguments) {
            if (!known_ref(value)) {
                return false;
            }
        }
        for (const ValueSpec& value : member.returns) {
            if (!known_ref(value)) {
                return false;
            }
        }
    }
    return true;
}

/** @return True when every handle ref in the table names a known receiver. */
[[nodiscard]] consteval bool valid_refs() {
    for (const ReceiverSpec& receiver : kReceivers) {
        if (!known_refs(receiver.members)) {
            return false;
        }
    }
    for (const FieldGroup& group : kFieldGroups) {
        if (!known_refs(group.members)) {
            return false;
        }
    }
    for (const AtomKindSpec& kind : kAtomKinds) {
        for (const ValueSpec& value : kind.fields) {
            if (!known_ref(value)) {
                return false;
            }
        }
    }
    for (const ValueSpec& entry : kProgramEntries) {
        if (!known_ref(entry)) {
            return false;
        }
    }
    return true;
}

static_assert(valid_receivers());
static_assert(valid_events());
static_assert(valid_atoms());
static_assert(valid_program());
static_assert(valid_refs());

} // namespace sunrise::server::activity::mission::api
