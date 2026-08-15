#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/activity/runtime.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The current boot-flow step accessor, `BootFlow_GetStep_NoBubbleArg`* @ `0x7FF742AED510`.
 * Decrypts the manager global and returns `mgr + 912`, or -1 when it is null. Only the call's
 * displacement is wildcarded; the `mgr + 912` field offset makes the pattern unique.
 */
constexpr std::string_view kStepSignatureText =
    "48 83 EC 28 E8 ? ? ? ? 48 85 C0 74 0B 8B 80 90 03 00 00 48 83 C4 28 C3 83 C8 FF 48 83 C4 28 "
    "C3";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kStepSignature = signature<signature_length(kStepSignatureText)>(kStepSignatureText);

/** First step that loads the map with no player in it yet. */
constexpr std::int32_t kActivityLoadFirst = 33;
/** `activity:in_world`. The fade is armed by then, so a spawn now releases it. */
constexpr std::int32_t kInWorld = 38;

using GetStep = std::int64_t(__fastcall*)() noexcept;

hooking::detour::Handle g_handle{};
std::atomic<GetStep> g_original{nullptr};

/** Maps one returned client step and reports only phase changes. */
void note_step(std::int32_t step) noexcept {
    state::activity::WorldPhase phase = state::activity::WorldPhase::idle;
    const char* name = "idle";
    if (step == kInWorld) {
        phase = state::activity::WorldPhase::arrived;
        name = "arrived";
    } else if (step >= kActivityLoadFirst && step < kInWorld) {
        phase = state::activity::WorldPhase::transitioning;
        name = "transitioning";
    } else {
        // Off a destination, so the next load is a fresh arming and logs its own release line.
        rearm_fade_release();
    }

    const state::activity::WorldPhase previous = state::activity::world_phase();
    state::activity::note_world_phase(phase);
    if (phase == previous) {
        return;
    }
    if (phase == state::activity::WorldPhase::arrived) {
        release_world_fade();
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=world_phase result=observed step=%d phase=%s",
                                      step,
                                      name);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         std::string_view(line.data(), static_cast<std::size_t>(written)));
    }
}

/** Observes every engine read of the boot-flow step, including the first read of in-world. */
__declspec(noinline) std::int64_t __fastcall current_step() noexcept {
    const GetStep original = g_original.load(std::memory_order_acquire);
    const std::int64_t value = original != nullptr ? original() : -1;
    note_step(static_cast<std::int32_t>(value & 0xFFFFFFFF));
    return value;
}

} // namespace

/** Maps the client's own boot-flow step onto the world phase. */
void observe_world_step() noexcept {
    const GetStep read = g_original.load(std::memory_order_acquire);
    if (read == nullptr) {
        return;
    }
    note_step(static_cast<std::int32_t>(read() & 0xFFFFFFFF));
}

/** Finds the boot-flow step accessor. */
bool install_world_step() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kStepSignature, "bootflow_current_step");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=world_step result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&current_step)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=world_step result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<GetStep>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=world_step result=ok");
    return true;
}

/** Clears the boot-flow step accessor it found. */
void uninstall_world_step() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
