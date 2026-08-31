#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "client/player/player_console.h"
#include "client/player/player_position.h"
#include "client/player/player_settings_store.h"
#include "client/hooks/teleport/runtime.h"
#include "core/console/registry/console_registry.h"
#include "server/bap/runtime.h"
#include "state/activity/membership/activity_membership_query.h"
#include "state/activity/runtime.h"

namespace {

namespace console = sunrise::core::console;
namespace registry = sunrise::core::console::registry;
namespace player = sunrise::client::player;
namespace activity = sunrise::state::activity;
namespace teleport = sunrise::client::hooks::teleport;

std::array<registry::Descriptor, 8> g_entries{};
std::size_t g_entryCount{};
player::Settings g_settings{};
player::position::Snapshot g_player{};
teleport::PositionRequestStatus g_request{};
std::uint64_t g_regionSession{};
bool g_privatePresent{};
sunrise::server::bap::ActivitySnapshot g_private{};
activity::SessionSnapshot g_session{};
bool g_sessionPresent{};
bool g_bindingMatches{};
bool g_requestAccepted{};
std::uint64_t g_nextRequestSequence{1};
std::uint64_t g_requestCalls{};

[[nodiscard]] const registry::Descriptor* find(std::string_view name) noexcept {
    for (std::size_t index = 0; index < g_entryCount; ++index) {
        if (g_entries[index].name == name) {
            return &g_entries[index];
        }
    }
    return nullptr;
}

[[nodiscard]] const console::Row* row(const console::Result& result,
                                      std::string_view key) noexcept {
    for (std::size_t index = 0; index < result.rowCount; ++index) {
        const console::Row& candidate = result.rows[index];
        if (std::string_view(candidate.key.data(), candidate.keyLength) == key) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string_view row_text(const console::Result& result,
                                        std::string_view key) noexcept {
    const console::Row* found = row(result, key);
    return found == nullptr ? std::string_view{}
                            : std::string_view{found->value.text.data(), found->value.textLength};
}

void reset_fakes() noexcept {
    g_player = {};
    g_request = {};
    g_regionSession = 0;
    g_privatePresent = false;
    g_private = {};
    g_session = {};
    g_sessionPresent = false;
    g_bindingMatches = false;
    g_requestAccepted = false;
    g_nextRequestSequence = 1;
    g_requestCalls = 0;
}

} // namespace

namespace sunrise::core::console::registry {

RegistrationResult register_entries(std::span<const Descriptor> descriptors) noexcept {
    g_entryCount = descriptors.size();
    for (std::size_t index = 0; index < g_entryCount; ++index) {
        g_entries[index] = descriptors[index];
    }
    return RegistrationResult::registered;
}

std::size_t unregister_prefix(std::string_view) noexcept {
    g_entryCount = 0;
    return 0;
}

} // namespace sunrise::core::console::registry

namespace sunrise::client::player {

Settings get() noexcept {
    return g_settings;
}

bool publish(const Settings& settings) noexcept {
    g_settings = settings;
    return true;
}

namespace position {

Snapshot snapshot() noexcept {
    return g_player;
}

} // namespace position

} // namespace sunrise::client::player

namespace sunrise::client::hooks::teleport {

bool request_position(const Vector& position,
                      std::uint64_t activitySession,
                      std::uint64_t& sequence) noexcept {
    ++g_requestCalls;
    sequence = g_requestAccepted ? g_nextRequestSequence++ : 0;
    if (g_requestAccepted) {
        g_request = PositionRequestStatus{position,
                                          sequence,
                                          activitySession,
                                          PositionRequestPhase::pending,
                                          PositionRequestFailure::none};
    }
    return g_requestAccepted;
}

PositionRequestStatus position_request_status() noexcept {
    return g_request;
}

const char* position_request_phase_name(PositionRequestPhase) noexcept {
    return "pending";
}

const char* position_request_failure_name(PositionRequestFailure) noexcept {
    return "none";
}

} // namespace sunrise::client::hooks::teleport

namespace sunrise::server::bap {

bool snapshot_private_activity(ActivitySnapshot& output) noexcept {
    output = g_private;
    return g_privatePresent;
}

} // namespace sunrise::server::bap

namespace sunrise::state::activity {

bool snapshot_session(std::uint64_t sessionId, SessionSnapshot& output) noexcept {
    if (!g_sessionPresent || sessionId != g_session.binding.sessionId) {
        output = {};
        return false;
    }
    output = g_session;
    return true;
}

bool binding_matches(const SessionBinding& binding) noexcept {
    return g_bindingMatches && binding.sessionId == g_session.binding.sessionId
           && binding.createdRevision == g_session.binding.createdRevision;
}

namespace membership {

std::uint64_t live_region_session(std::uint64_t fallback) noexcept {
    return g_regionSession == 0 ? fallback : g_regionSession;
}

} // namespace membership

} // namespace sunrise::state::activity

int main() {
    reset_fakes();
    assert(player::console::initialize());

    const registry::Descriptor* ammo = find("player.infinite_ammo");
    const registry::Descriptor* position = find("player.position");
    const registry::Descriptor* teleport = find("player.teleport");
    assert(ammo != nullptr && ammo->kind == registry::Kind::variable
           && ammo->type == console::Type::boolean && ammo->read != nullptr
           && ammo->write != nullptr);
    assert(position != nullptr && position->kind == registry::Kind::command
           && position->arguments.empty() && position->invoke != nullptr);
    assert(teleport != nullptr && teleport->kind == registry::Kind::command
           && teleport->arguments.size() == 3 && teleport->invoke != nullptr
           && teleport->arguments[0].minimum < 0.0
           && teleport->arguments[0].maximum >= std::numeric_limits<float>::max());

    console::Value value{};
    assert(ammo->read(value) && value.boolean == false);
    value.boolean = true;
    assert(ammo->write(value) == console::Status::ok);
    assert(ammo->read(value) && value.boolean);

    console::Result result{};
    result = {};
    position->invoke({}, result);
    assert(result.status == console::Status::refused && result.rowCount == 0);

    g_player.present = true;
    g_player.position = {1.0F, 2.0F, 3.0F};
    result = {};
    position->invoke({}, result);
    assert(result.status == console::Status::refused && result.rowCount == 0);

    g_sessionPresent = true;
    constexpr std::uint64_t kPrivateSession = 0x9EAA300100200001ULL;
    constexpr std::uint64_t kRegionSession = 0x9EAA300100200002ULL;
    g_session.binding.sessionId = kPrivateSession;
    g_session.binding.createdRevision = 7;
    g_bindingMatches = true;
    g_privatePresent = true;
    g_private.binding.sessionId = kPrivateSession;
    g_private.binding.createdRevision = 7;
    g_regionSession = kRegionSession;
    result = {};
    position->invoke({}, result);
    assert(result.status == console::Status::ok);
    assert(row_text(result, "activity_session") == "11433003384888623105");
    assert(row(result, "x") != nullptr && row(result, "x")->value.real == 1.0);
    assert(row_text(result, "request_sequence") == "0");

    g_private.binding.createdRevision = 8;
    result = {};
    position->invoke({}, result);
    assert(result.status == console::Status::refused);

    g_privatePresent = false;
    g_private = {};
    g_session.binding.sessionId = kRegionSession;
    g_session.binding.createdRevision = 4;
    g_regionSession = kRegionSession;
    result = {};
    position->invoke({}, result);
    assert(result.status == console::Status::ok
           && row_text(result, "activity_session") == "11433003384888623106");

    std::array<console::Value, 3> arguments{};
    arguments[0].type = console::Type::real;
    arguments[0].real = 10.5;
    arguments[1].type = console::Type::real;
    arguments[1].real = -20.25;
    arguments[2].type = console::Type::real;
    arguments[2].real = 30.75;
    g_requestAccepted = true;
    g_player.present = false;
    result = {};
    teleport->invoke(arguments, result);
    assert(result.status == console::Status::ok && g_requestCalls == 1);
    assert(row_text(result, "activity_session") == "11433003384888623106");
    assert(row_text(result, "request_sequence") == "1");
    assert(row(result, "z")->value.real == 30.75);

    g_regionSession = 0;
    result = {};
    teleport->invoke(arguments, result);
    assert(result.status == console::Status::refused && g_requestCalls == 1);

    player::console::shutdown();
}
