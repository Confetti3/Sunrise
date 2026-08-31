#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <string_view>

#include "client/hooks/retail_log/retail_log_enqueue_observer.h"
#include "client/hooks/squad_reference_probe/squad_reference_probe.h"
#include "core/console/registry/console_registry.h"
#include "server/bap/encrypted/push/activity/activity_roster_research.h"
#include "server/gameplay/mission/mission_console.h"
#include "server/gameplay/physics/host/physics_session.h"

namespace console = sunrise::core::console;
namespace registry = sunrise::core::console::registry;
namespace mission = sunrise::server::gameplay::mission;
namespace roster = sunrise::server::bap::encrypted::push::activity;

namespace {
std::array<registry::Descriptor, 16> g_entries{};
std::size_t g_entryCount{};
std::uint64_t g_requestToken{};
roster::EntitySlotRepublishStatus g_republishStatus{};

const registry::Descriptor* find(std::string_view name) {
    for (std::size_t index = 0; index < g_entryCount; ++index) {
        if (g_entries[index].name == name) return &g_entries[index];
    }
    return nullptr;
}

const console::Row* row(const console::Result& result, std::string_view key) {
    for (std::size_t index = 0; index < result.rowCount; ++index) {
        const console::Row& candidate = result.rows[index];
        if (std::string_view(candidate.key.data(), candidate.keyLength) == key) return &candidate;
    }
    return nullptr;
}
}

namespace sunrise::core::console::registry {
RegistrationResult register_entries(std::span<const Descriptor> descriptors) noexcept {
    assert(descriptors.size() <= g_entries.size());
    g_entryCount = descriptors.size();
    for (std::size_t index = 0; index < g_entryCount; ++index) g_entries[index] = descriptors[index];
    return RegistrationResult::registered;
}
std::size_t unregister_prefix(std::string_view prefix) noexcept {
    assert(prefix == mission::console::kPrefix);
    const std::size_t removed = g_entryCount;
    g_entryCount = 0;
    return removed;
}
}

namespace sunrise::server::bap::encrypted::push::activity {
std::uint64_t request_entity_slot_republish() noexcept { return g_requestToken; }
EntitySlotRepublishStatus entity_slot_republish_status() noexcept { return g_republishStatus; }
TrostlandSpawnerResearch trostland_spawner_research() noexcept { return {}; }
void set_trostland_spawner_generation(std::uint32_t) noexcept {}
void set_trostland_spawner_delta(std::uint32_t) noexcept {}
void set_trostland_spawner_reset(bool) noexcept {}
void request_trostland_spawner_reconciliation() noexcept {}
std::uint64_t request_glimmer_intro() noexcept { return 1; }
}

namespace sunrise::server::gameplay::physics::host::session {
MissionReloadStatus mission_reload_status() noexcept { return {}; }
std::uint64_t request_mission_reload() noexcept { return 1; }
std::uint64_t request_mission_trigger(std::uint64_t) noexcept { return 1; }
std::uint64_t request_mission_content_signal(std::uint64_t) noexcept { return 1; }
}

namespace sunrise::client::hooks::retail_log {
std::uint64_t rearm_sobject_capture() noexcept { return 1; }
SobjectCaptureStatus sobject_capture_status() noexcept { return {}; }
std::uint64_t capture_sobject_function(std::uintptr_t) noexcept { return 1; }
}

namespace sunrise::client::hooks::squad_reference_probe {
RuntimeSnapshot runtime_snapshot() noexcept { return {}; }
}

int main() {
    assert(mission::console::initialize());
    const auto* request = find("mission.entity_slots_republish");
    const auto* statusCommand = find("mission.entity_slots_republish_status");
    assert(request != nullptr && request->kind == registry::Kind::command
           && request->arguments.empty() && request->invoke != nullptr);
    assert(statusCommand != nullptr && statusCommand->kind == registry::Kind::command
           && statusCommand->arguments.empty() && statusCommand->invoke != nullptr);

    g_republishStatus.noPrivateRejected = 4;
    console::Result result{};
    request->invoke({}, result);
    assert(result.status == console::Status::refused);
    assert(row(result, "token") != nullptr && row(result, "token")->value.integer == 0);
    assert(row(result, "no_private_rejected") != nullptr
           && row(result, "no_private_rejected")->value.integer == 4);
    assert(row(result, "requested") != nullptr && row(result, "bound") != nullptr
           && row(result, "staged") != nullptr && row(result, "delivered") != nullptr
           && row(result, "discarded") != nullptr && row(result, "stale_rejected") != nullptr
           && row(result, "public_rejected") != nullptr && row(result, "encode_failed") != nullptr
           && row(result, "pending_token") != nullptr && row(result, "staged_token") != nullptr
           && row(result, "delivered_token") != nullptr && row(result, "pending_binding") != nullptr
           && row(result, "staged_binding") != nullptr);

    g_requestToken = 55;
    g_republishStatus = {};
    g_republishStatus.requested = 7;
    g_republishStatus.bound = 6;
    g_republishStatus.staged = 5;
    g_republishStatus.delivered = 4;
    g_republishStatus.discarded = 3;
    g_republishStatus.staleRejected = 2;
    g_republishStatus.noPrivateRejected = 1;
    g_republishStatus.publicRejected = 8;
    g_republishStatus.encodeFailed = 9;
    g_republishStatus.pendingToken = 55;
    g_republishStatus.stagedToken = 54;
    g_republishStatus.deliveredToken = 53;
    g_republishStatus.pendingBindingGeneration = 22;
    g_republishStatus.stagedBindingGeneration = 21;
    result = {};
    request->invoke({}, result);
    assert(result.status == console::Status::ok
           && row(result, "token")->value.integer == 55
           && row(result, "pending_token")->value.integer == 55);

    result = {};
    statusCommand->invoke({}, result);
    assert(result.status == console::Status::ok && result.rowCount == 14);
    assert(row(result, "requested")->value.integer == 7);
    assert(row(result, "bound")->value.integer == 6);
    assert(row(result, "staged")->value.integer == 5);
    assert(row(result, "delivered")->value.integer == 4);
    assert(row(result, "discarded")->value.integer == 3);
    assert(row(result, "stale_rejected")->value.integer == 2);
    assert(row(result, "no_private_rejected")->value.integer == 1);
    assert(row(result, "public_rejected")->value.integer == 8);
    assert(row(result, "encode_failed")->value.integer == 9);
    assert(row(result, "pending_token")->value.integer == 55);
    assert(row(result, "staged_token")->value.integer == 54);
    assert(row(result, "delivered_token")->value.integer == 53);
    assert(row(result, "pending_binding")->value.integer == 22);
    assert(row(result, "staged_binding")->value.integer == 21);

    mission::console::shutdown();
    assert(g_entryCount == 0);
    return 0;
}
