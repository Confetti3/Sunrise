#include "mission_console.h"

#include <array>
#include <cstdint>
#include <cstdio>

#include "../../../client/hooks/retail_log/retail_log_enqueue_observer.h"
#include "../../../client/hooks/squad_reference_probe/squad_reference_probe.h"
#include "../../../core/console/registry/console_registry.h"
#include "../../bap/encrypted/push/activity/activity_roster_research.h"
#include "../physics/host/physics_session.h"

namespace sunrise::server::gameplay::mission::console {
namespace {

namespace entry = core::console;
namespace registry = core::console::registry;
namespace roster = ::sunrise::server::bap::encrypted::push::activity;
namespace retail = ::sunrise::client::hooks::retail_log;

constexpr std::array<registry::Argument, 1> kTriggerArguments{
    registry::Argument{"id", "Authored proximity interaction id.", entry::Type::text}};
constexpr std::array<registry::Argument, 1> kContentStepArguments{
    registry::Argument{"id", "Allowlisted authored content-step id.", entry::Type::text}};
constexpr std::array<registry::Argument, 1> kCodeCaptureArguments{
    registry::Argument{"rva", "Main-image executable RVA to capture as a function or raw window.",
                       entry::Type::integer, 0, 4294967295.0}};

[[nodiscard]] entry::Value integer_value(std::uint64_t value) noexcept {
    entry::Value result{};
    result.type = entry::Type::integer;
    result.integer = static_cast<std::int64_t>(value);
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

[[nodiscard]] entry::Value hex_value(std::uint64_t value) noexcept {
    std::array<char, 19> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "0x%016llX",
                                      static_cast<unsigned long long>(value));
    return written > 0 ? text_value({buffer.data(), static_cast<std::size_t>(written)})
                       : text_value("");
}

[[nodiscard]] std::uint64_t id_of(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) hash = (hash ^ byte) * 1099511628211ULL;
    return hash == 0 ? 1 : hash;
}

void append_status(entry::Result& output) noexcept {
    const physics::host::session::MissionReloadStatus status =
        physics::host::session::mission_reload_status();
    static_cast<void>(entry::add_row(output, "requested", integer_value(status.requested)));
    static_cast<void>(entry::add_row(output, "completed", integer_value(status.completed)));
    static_cast<void>(entry::add_row(output, "active_worlds", integer_value(status.activeWorlds)));
    static_cast<void>(entry::add_row(output, "trigger_requested", integer_value(status.triggerRequested)));
    static_cast<void>(entry::add_row(output, "trigger_completed", integer_value(status.triggerCompleted)));
    static_cast<void>(entry::add_row(output, "program_hash", hex_value(status.programHash)));
    static_cast<void>(entry::add_row(output, "mission_state", integer_value(status.missionState)));
    static_cast<void>(entry::add_row(output, "objective_0", integer_value(status.objective0)));
    static_cast<void>(entry::add_row(output, "activated_waves", integer_value(status.activatedWaveCount)));
}

void reload(std::span<const entry::Value>, entry::Result& output) noexcept {
    static_cast<void>(physics::host::session::request_mission_reload());
    output.status = entry::Status::ok;
    entry::set_summary(output, "Mission reload queued for every active world.");
    append_status(output);
}

void status(std::span<const entry::Value>, entry::Result& output) noexcept {
    output.status = entry::Status::ok;
    entry::set_summary(output, "Current mission reload state.");
    append_status(output);
}

void append_capture_status(entry::Result& output) noexcept {
    const retail::SobjectCaptureStatus status = retail::sobject_capture_status();
    static_cast<void>(entry::add_row(output, "generation", integer_value(status.generation)));
    static_cast<void>(entry::add_row(output, "wide_armed", boolean_value(status.wideArmed)));
    static_cast<void>(
        entry::add_row(output, "wide_capturing", boolean_value(status.wideCapturing)));
    static_cast<void>(
        entry::add_row(output, "wide_rearming", boolean_value(status.wideRearming)));
    static_cast<void>(
        entry::add_row(output, "background_failures", integer_value(status.backgroundFailures)));
    static_cast<void>(
        entry::add_row(output, "recent_failures", integer_value(status.recentFailures)));
}

void capture_arm(std::span<const entry::Value>, entry::Result& output) noexcept {
    const std::uint64_t generation = retail::rearm_sobject_capture();
    output.status = generation == 0 ? entry::Status::refused : entry::Status::ok;
    entry::set_summary(output,
                       generation == 0
                           ? "Wide sobject capture is currently writing code rows."
                           : "Wide sobject code capture armed without a client restart.");
    append_capture_status(output);
}

void capture_status(std::span<const entry::Value>, entry::Result& output) noexcept {
    output.status = entry::Status::ok;
    entry::set_summary(output, "Current wide sobject capture state.");
    append_capture_status(output);
}

void capture_code(std::span<const entry::Value> arguments, entry::Result& output) noexcept {
    const std::uintptr_t rva = static_cast<std::uintptr_t>(arguments[0].integer);
    const std::uint64_t generation = retail::capture_sobject_function(rva);
    output.status = generation == 0 ? entry::Status::refused : entry::Status::ok;
    entry::set_summary(output,
                       generation == 0
                           ? "Function capture refused: logging or executable function unavailable."
                           : "Function code rows written to the live Sunrise log.");
    static_cast<void>(entry::add_row(output, "capture", integer_value(generation)));
    static_cast<void>(entry::add_row(output, "rva", hex_value(rva)));
}

void trigger(std::span<const entry::Value> arguments, entry::Result& output) noexcept {
    const std::string_view name{arguments[0].text.data(), arguments[0].textLength};
    const std::uint64_t request = physics::host::session::request_mission_trigger(id_of(name));
    output.status = request == 0 ? entry::Status::refused : entry::Status::ok;
    entry::set_summary(output, request == 0 ? "Trigger input was refused."
                                            : "Trigger-enter input queued for the mission worker.");
    static_cast<void>(entry::add_row(output, "request", integer_value(request)));
    static_cast<void>(entry::add_row(output, "trigger", hex_value(id_of(name))));
}

void signal(std::span<const entry::Value> arguments, entry::Result& output) noexcept {
    const std::string_view name{arguments[0].text.data(), arguments[0].textLength};
    const std::uint64_t request =
        physics::host::session::request_mission_content_signal(id_of(name));
    output.status = request == 0 ? entry::Status::refused : entry::Status::ok;
    entry::set_summary(output, request == 0 ? "Content signal was refused."
                                            : "Content observation queued for the mission worker.");
    static_cast<void>(entry::add_row(output, "request", integer_value(request)));
    static_cast<void>(entry::add_row(output, "signal", hex_value(id_of(name))));
}

void spawner_status(std::span<const entry::Value>, entry::Result& output) noexcept {
    const roster::TrostlandSpawnerResearch wire = roster::trostland_spawner_research();
    const ::sunrise::client::hooks::squad_reference_probe::RuntimeSnapshot runtime =
        ::sunrise::client::hooks::squad_reference_probe::runtime_snapshot();
    static_cast<void>(entry::add_row(output, "observed_generation", integer_value(wire.observedGeneration)));
    static_cast<void>(entry::add_row(output, "observed_delta", hex_value(wire.observedDelta)));
    static_cast<void>(entry::add_row(output, "manual_generation", integer_value(wire.manualGeneration)));
    static_cast<void>(entry::add_row(output, "apply_calls", integer_value(runtime.applyCalls)));
    static_cast<void>(entry::add_row(output, "resolve_calls", integer_value(runtime.resolveCalls)));
    static_cast<void>(entry::add_row(output, "build_request_calls", integer_value(runtime.buildRequestCalls)));
    static_cast<void>(entry::add_row(output, "requested_0", integer_value(runtime.requestedFirst)));
    static_cast<void>(entry::add_row(output, "requested_1", integer_value(runtime.requestedSecond)));
    static_cast<void>(entry::add_row(output, "pending", integer_value(runtime.pending)));
    static_cast<void>(entry::add_row(output, "last_active_instance", hex_value(runtime.lastActiveInstance)));
    static_cast<void>(entry::add_row(output, "decoded_slot_count", integer_value(runtime.decodedSlotCount)));
    static_cast<void>(entry::add_row(output, "decoded_generation", integer_value(runtime.decodedGeneration)));
    static_cast<void>(entry::add_row(output, "decoded_requested_0", integer_value(runtime.decodedRequestedFirst)));
    static_cast<void>(entry::add_row(output, "decoded_requested_1", integer_value(runtime.decodedRequestedSecond)));
    static_cast<void>(entry::add_row(output, "decoded_mode", integer_value(runtime.decodedMode)));
    static_cast<void>(entry::add_row(output, "decoded_active", boolean_value(runtime.decodedActive)));
    output.status = entry::Status::ok;
    entry::set_summary(output, "Live Trostland spawner wire and runtime probe state.");
}

void append_entity_slot_republish_status(entry::Result& output) noexcept {
    const roster::EntitySlotRepublishStatus status = roster::entity_slot_republish_status();
    static_cast<void>(entry::add_row(output, "requested", integer_value(status.requested)));
    static_cast<void>(entry::add_row(output, "bound", integer_value(status.bound)));
    static_cast<void>(entry::add_row(output, "staged", integer_value(status.staged)));
    static_cast<void>(entry::add_row(output, "delivered", integer_value(status.delivered)));
    static_cast<void>(entry::add_row(output, "discarded", integer_value(status.discarded)));
    static_cast<void>(entry::add_row(output, "stale_rejected", integer_value(status.staleRejected)));
    static_cast<void>(entry::add_row(output, "no_private_rejected", integer_value(status.noPrivateRejected)));
    static_cast<void>(entry::add_row(output, "public_rejected", integer_value(status.publicRejected)));
    static_cast<void>(entry::add_row(output, "encode_failed", integer_value(status.encodeFailed)));
    static_cast<void>(entry::add_row(output, "pending_token", integer_value(status.pendingToken)));
    static_cast<void>(entry::add_row(output, "staged_token", integer_value(status.stagedToken)));
    static_cast<void>(entry::add_row(output, "delivered_token", integer_value(status.deliveredToken)));
    static_cast<void>(entry::add_row(output,
                                     "pending_binding",
                                     integer_value(status.pendingBindingGeneration)));
    static_cast<void>(entry::add_row(output,
                                     "staged_binding",
                                     integer_value(status.stagedBindingGeneration)));
}

void entity_slot_republish(std::span<const entry::Value>, entry::Result& output) noexcept {
    const std::uint64_t token = roster::request_entity_slot_republish();
    output.status = token == 0 ? entry::Status::refused : entry::Status::ok;
    entry::set_summary(
        output,
        token == 0
            ? "Entity-slot republish refused: no exact private-current activity binding."
            : "Manual diagnostic queued; duplicate type-0 client semantics remain unproven.");
    static_cast<void>(entry::add_row(output, "token", integer_value(token)));
    append_entity_slot_republish_status(output);
}

void entity_slot_republish_status(std::span<const entry::Value>, entry::Result& output) noexcept {
    output.status = entry::Status::ok;
    entry::set_summary(output, "Manual private-current entity-slot republish diagnostic state.");
    append_entity_slot_republish_status(output);
}

void content_step(std::span<const entry::Value> arguments, entry::Result& output) noexcept {
    const std::string_view name{arguments[0].text.data(), arguments[0].textLength};
    if (name != "glimmer_intro") {
        output.status = entry::Status::refused;
        entry::set_summary(output, "Unknown content step for build 86657.");
        return;
    }
    const std::uint64_t token = roster::request_glimmer_intro();
    output.status = token == 0 ? entry::Status::refused : entry::Status::ok;
    entry::set_summary(output, token == 0 ? "Content step was refused."
                                          : "Glimmer intro step queued.");
    static_cast<void>(entry::add_row(output, "token", integer_value(token)));
}

[[nodiscard]] registry::Descriptor command(std::string_view name,
                                           std::string_view help,
                                           registry::InvokeCallback invoke) noexcept {
    registry::Descriptor descriptor{};
    descriptor.name = name;
    descriptor.help = help;
    descriptor.kind = registry::Kind::command;
    descriptor.invoke = invoke;
    return descriptor;
}

} // namespace

bool initialize() noexcept {
    registry::Descriptor inject =
        command("mission.trigger", "Queue one trigger-enter edge by authored interaction id.", &trigger);
    inject.arguments = kTriggerArguments;
    registry::Descriptor content = command(
        "mission.content_step", "Queue one allowlisted build-scoped content step.", &content_step);
    content.arguments = kContentStepArguments;
    registry::Descriptor captureCode = command(
        "mission.capture_code", "Capture a mapped function or bounded raw executable window by RVA.",
        &capture_code);
    captureCode.arguments = kCodeCaptureArguments;
    const std::array entries{
        command("mission.capture_arm", "Re-arm the one-shot wide sobject code capture.", &capture_arm),
        captureCode,
        command("mission.capture_status", "Report wide sobject capture counters.", &capture_status),
        command("mission.entity_slots_republish",
                "Queue one private-current held-slot type-0 diagnostic before the next roster.",
                &entity_slot_republish),
        command("mission.entity_slots_republish_status",
                "Report entity-slot diagnostic tokens and delivery counters.",
                &entity_slot_republish_status),
        command("mission.reload", "Recompile and reopen every active Lua mission world.", &reload),
        command("mission.spawner_status", "Report live Trostland spawner research state.", &spawner_status),
        content,
        command("mission.status", "Report live mission reload counters and active worlds.", &status),
        [&]() {
            auto descriptor = command("mission.signal", "Queue one named content observation.", &signal);
            descriptor.arguments = kTriggerArguments;
            return descriptor;
        }(),
        inject,
    };
    return registry::register_entries(entries) == registry::RegistrationResult::registered;
}

void shutdown() noexcept {
    static_cast<void>(registry::unregister_prefix(kPrefix));
}

} // namespace sunrise::server::gameplay::mission::console
