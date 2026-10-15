#pragma once

#include <string_view>

namespace sunrise::server::activity::mission::lua_vm::detail {

// Every metatable name the mission API registers, and the mission ABI ids two intents are named by.
inline constexpr char kActivityMetatable[] = "sunrise.sdk.activity";
inline constexpr char kActivityBindingTagCollectionMetatable[] =
    "sunrise.sdk.activity_binding_tags";
// Every binding locator of the bound activity.
inline constexpr char kActivityBindingLocatorCollectionMetatable[] =
    "sunrise.sdk.activity_binding_locators";
// One binding locator row, then the catalog collections a program indexes.
inline constexpr char kActivityBindingLocatorMetatable[] = "sunrise.sdk.activity_binding_locator";
inline constexpr char kSquadCollectionMetatable[] = "sunrise.sdk.squads";
inline constexpr char kSceneCollectionMetatable[] = "sunrise.sdk.authored_scenes";
inline constexpr char kSlotCollectionMetatable[] = "sunrise.sdk.slots";
inline constexpr char kMessageCollectionMetatable[] = "sunrise.sdk.activity_messages";
inline constexpr char kMessageFieldCollectionMetatable[] = "sunrise.sdk.activity_message_fields";
inline constexpr char kContextMetatable[] = "sunrise.mission.context";
inline constexpr char kStateMetatable[] = "sunrise.mission.state";
inline constexpr char kLifetimeMetatable[] = "sunrise.mission.lifetime";
inline constexpr char kModConfigMetatable[] = "sunrise.mission.mod_config";

// One event view for every kind; the kind picks the fields.
inline constexpr char kEventMetatable[] = "sunrise.mission.event";

// Metatable names for one row of each catalog collection above.
inline constexpr char kSquadMetatable[] = "sunrise.sdk.squad";
inline constexpr char kSceneMetatable[] = "sunrise.sdk.authored_scene";
inline constexpr char kSlotMetatable[] = "sunrise.sdk.slot";
inline constexpr char kMessageMetatable[] = "sunrise.sdk.activity_message";
inline constexpr char kMessageFieldMetatable[] = "sunrise.sdk.activity_message_field";

} // namespace sunrise::server::activity::mission::lua_vm::detail
