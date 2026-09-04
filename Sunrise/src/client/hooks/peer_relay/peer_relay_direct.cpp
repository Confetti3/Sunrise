/**
 * Forces the peer channel to connect directly instead of through a NAT relay.
 * The channel manager suppresses its relay only when this call's fourth argument is set, and
 * every stock caller passes zero, so the gameplay peer channel always relays. On a loopback
 * host with no relay server that channel never connects. Forcing the flag makes the client
 * secure the channel directly against the endpoint the host binds.
 */

#include "peer_relay_direct.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"

namespace sunrise::client::hooks::peer_relay {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * `NetChannel_RequestConnectForFamily`. Its fourth argument is the suppress-relay flag; the body
 * tests the channel flag byte at `+0x3068` and its stride constant is `0x41F0`.
 */
constexpr std::string_view kConnectSignatureText =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 0F B6 F9 49 63 D8 8B F2 E8 ? ? ? ? 4C 69 C3 "
    "F0 41 00 00 49 81 C0 A8 00 00 00 4C 03 C0 41 F6 80 68 30 00 00 01";
constexpr auto kConnectSignature =
    signature<signature_length(kConnectSignatureText)>(kConnectSignatureText);

/** The suppress-relay flag value that selects a direct connect. */
constexpr std::uint64_t kSuppressRelay = 1;

/** The channel connect request this detour replaces. */
using ConnectForFamily = std::int64_t(__fastcall*)(void*,
                                                   std::uint32_t,
                                                   std::int32_t,
                                                   std::uint64_t);

hooking::detour::Handle g_handle{};
std::atomic_bool g_reported{false};

/** Writes the one line naming which answer this run uses. */
void report(const char* result) noexcept {
    if (g_reported.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=peer_relay stage=connect result=%s", result);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Requests the channel connect, forcing the relay suppressed when the direct mode is on.
 * @param container Borrowed channel container, passed through.
 * @param family Family index, passed through.
 * @param slot Channel slot index, passed through.
 * @param suppressRelay The stock flag; replaced with the direct value when the mode is on.
 * @return The original result.
 */
__declspec(noinline)
/** Detour body: forces the direct-connect flag when the setting is on. */
std::int64_t __fastcall connect_for_family(void* container,
                                           std::uint32_t family,
                                           std::int32_t slot,
                                           std::uint64_t suppressRelay) noexcept {
    const auto original = reinterpret_cast<ConnectForFamily>(g_handle.original);
    if (original == nullptr) {
        return 0;
    }
    if (core::settings::get().client.suppressPeerRelay) {
        report("direct");
        return original(container, family, slot, kSuppressRelay);
    }
    report("native");
    return original(container, family, slot, suppressRelay);
}

} // namespace

/** Attaches the peer-channel direct-connect forcing. */
bool install() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kConnectSignature, "peer_relay_connect");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=peer_relay stage=connect result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&connect_for_family)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=peer_relay stage=connect result=fail reason=attach");
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=peer_relay stage=connect result=ok");
    return true;
}

/** Detaches the peer-channel direct-connect forcing. */
void uninstall() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_reported.store(false, std::memory_order_release);
}

} // namespace sunrise::client::hooks::peer_relay
