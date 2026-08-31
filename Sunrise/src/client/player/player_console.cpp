#include "player_console.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>

#include "../../core/console/registry/console_registry.h"
#include "../../server/bap/runtime.h"
#include "../../state/activity/membership/activity_membership_query.h"
#include "../../state/activity/runtime.h"
#include "../hooks/teleport/runtime.h"
#include "player_position.h"
#include "player_settings_store.h"

namespace sunrise::client::player::console {
namespace {

namespace entry = core::console;
namespace registry = core::console::registry;
namespace activity = state::activity;
namespace teleport = hooks::teleport;

constexpr double kCoordinateLimit =
    static_cast<double>((std::numeric_limits<float>::max)());
constexpr std::array<registry::Argument, 3> kTeleportArguments{
    registry::Argument{"x", "Finite world-space X coordinate.", entry::Type::real,
                       -kCoordinateLimit, kCoordinateLimit},
    registry::Argument{"y", "Finite world-space Y coordinate.", entry::Type::real,
                       -kCoordinateLimit, kCoordinateLimit},
    registry::Argument{"z", "Finite world-space Z coordinate.", entry::Type::real,
                       -kCoordinateLimit, kCoordinateLimit}};

[[nodiscard]] entry::Value real_value(float value) noexcept {
    entry::Value result{};
    result.type = entry::Type::real;
    result.real = static_cast<double>(value);
    return result;
}

[[nodiscard]] entry::Value identifier_value(std::uint64_t value) noexcept {
    std::array<char, 21> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    entry::Value result{};
    result.type = entry::Type::text;
    if (converted.ec == std::errc{}) {
        entry::store_text(
            std::string_view{digits.data(), static_cast<std::size_t>(converted.ptr - digits.data())},
            result.text,
            result.textLength);
    }
    return result;
}

[[nodiscard]] entry::Value boolean_value(bool value) noexcept {
    entry::Value result{};
    result.type = entry::Type::boolean;
    result.boolean = value;
    return result;
}

[[nodiscard]] entry::Value text_value(std::string_view value) noexcept {
    entry::Value result{};
    result.type = entry::Type::text;
    entry::store_text(value, result.text, result.textLength);
    return result;
}

/** Picks the same current session used by spawn inspection: private binding first, region second. */
[[nodiscard]] bool current_activity_session(std::uint64_t& output) noexcept {
    output = activity::kAbsentSessionId;
    activity::SessionSnapshot session{};
    server::bap::ActivitySnapshot privateActivity{};
    if (server::bap::snapshot_private_activity(privateActivity)) {
        if (privateActivity.binding.sessionId == activity::kAbsentSessionId
            || !activity::snapshot_session(privateActivity.binding.sessionId, session)
            || session.binding.sessionId != privateActivity.binding.sessionId
            || session.binding.createdRevision != privateActivity.binding.createdRevision
            || !activity::binding_matches(session.binding)) {
            return false;
        }
        output = session.binding.sessionId;
        return true;
    }

    const std::uint64_t regionSession =
        activity::membership::live_region_session(activity::kAbsentSessionId);
    if (regionSession == activity::kAbsentSessionId
        || !activity::snapshot_session(regionSession, session)
        || !activity::binding_matches(session.binding)) {
        return false;
    }
    output = session.binding.sessionId;
    return true;
}

void add_request_status(entry::Result& output,
                        const teleport::PositionRequestStatus& request) noexcept {
    static_cast<void>(entry::add_row(output, "request_sequence", identifier_value(request.sequence)));
    static_cast<void>(entry::add_row(output, "request_session", identifier_value(request.activitySession)));
    static_cast<void>(entry::add_row(
        output, "request_phase", text_value(teleport::position_request_phase_name(request.phase))));
    static_cast<void>(entry::add_row(
        output, "request_failure", text_value(teleport::position_request_failure_name(request.failure))));
}

/** Reads whether ammunition is unlimited. */
[[nodiscard]] bool read_infinite_ammo(entry::Value& output) noexcept {
    output = entry::Value{};
    output.type = entry::Type::boolean;
    output.boolean = get().infiniteAmmoEnabled;
    return true;
}

/**
 * Writes whether ammunition is unlimited.
 *
 * The whole set is published back because that is the only interface the module offers. Nothing
 * can interleave: console handlers and the pages both run on the thread that draws.
 */
[[nodiscard]] entry::Status write_infinite_ammo(const entry::Value& value) noexcept {
    Settings settings = get();
    settings.infiniteAmmoEnabled = value.boolean;
    return publish(settings) ? entry::Status::ok : entry::Status::failed;
}

void player_position(std::span<const entry::Value>, entry::Result& output) noexcept {
    const position::Snapshot player = position::snapshot();
    if (!player.present) {
        output.status = entry::Status::refused;
        entry::set_summary(output, "No local player position is available.");
        return;
    }

    std::uint64_t activitySession = activity::kAbsentSessionId;
    if (!current_activity_session(activitySession)) {
        output.status = entry::Status::refused;
        entry::set_summary(output, "No current activity session is available.");
        return;
    }

    const teleport::PositionRequestStatus request = teleport::position_request_status();
    static_cast<void>(entry::add_row(output, "x", real_value(player.position[0])));
    static_cast<void>(entry::add_row(output, "y", real_value(player.position[1])));
    static_cast<void>(entry::add_row(output, "z", real_value(player.position[2])));
    static_cast<void>(entry::add_row(output, "activity_session", identifier_value(activitySession)));
    static_cast<void>(entry::add_row(
        output, "controlled_handle", identifier_value(player.controlledHandle)));
    static_cast<void>(entry::add_row(
        output, "controlled_handle_present", boolean_value(player.controlledHandlePresent)));
    add_request_status(output, request);
    output.status = entry::Status::ok;
    entry::set_summary(output, "Current local player position and teleport request state.");
}

void player_teleport(std::span<const entry::Value> arguments, entry::Result& output) noexcept {
    std::uint64_t activitySession = activity::kAbsentSessionId;
    if (!current_activity_session(activitySession)) {
        output.status = entry::Status::refused;
        entry::set_summary(output, "No current activity session is available.");
        return;
    }

    const teleport::Vector destination{static_cast<float>(arguments[0].real),
                                       static_cast<float>(arguments[1].real),
                                       static_cast<float>(arguments[2].real)};
    std::uint64_t sequence = 0;
    const bool queued = teleport::request_position(destination, activitySession, sequence);
    static_cast<void>(entry::add_row(output, "x", real_value(destination[0])));
    static_cast<void>(entry::add_row(output, "y", real_value(destination[1])));
    static_cast<void>(entry::add_row(output, "z", real_value(destination[2])));
    static_cast<void>(entry::add_row(output, "activity_session", identifier_value(activitySession)));
    static_cast<void>(entry::add_row(output, "request_sequence", identifier_value(sequence)));
    output.status = queued ? entry::Status::ok : entry::Status::refused;
    entry::set_summary(output, queued ? "Player teleport queued for the next validated sync."
                                      : "Player teleport request was refused.");
}

} // namespace

/** Publishes the player settings as console variables. */
bool initialize() noexcept {
    registry::Descriptor ammo{};
    ammo.name = "player.infinite_ammo";
    ammo.help = "Whether magazines stop consuming ammunition.";
    ammo.kind = registry::Kind::variable;
    ammo.type = entry::Type::boolean;
    ammo.read = &read_infinite_ammo;
    ammo.write = &write_infinite_ammo;

    registry::Descriptor position{};
    position.name = "player.position";
    position.help = "Reports the local player position and teleport request state.";
    position.kind = registry::Kind::command;
    position.invoke = &player_position;

    registry::Descriptor teleport{};
    teleport.name = "player.teleport";
    teleport.help = "Queues an absolute local-player position for the current activity session.";
    teleport.kind = registry::Kind::command;
    teleport.arguments = kTeleportArguments;
    teleport.invoke = &player_teleport;

    const std::array entries{ammo, position, teleport};
    return registry::register_entries(entries) == registry::RegistrationResult::registered;
}

/** Removes the player entries. */
void shutdown() noexcept {
    static_cast<void>(registry::unregister_prefix(kPrefix));
}

} // namespace sunrise::client::player::console
