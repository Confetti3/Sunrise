#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::server::script_host::protocol {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr wchar_t kDefaultPipeName[] = L"\\\\.\\pipe\\sunrise-script-host-v1";
inline constexpr wchar_t kDisabledEnvironment[] = L"SUNRISE_SCRIPT_HOST_DISABLED";
inline constexpr wchar_t kPipeEnvironment[] = L"SUNRISE_SCRIPT_HOST_PIPE";
inline constexpr std::size_t kMaximumPipePath = 260;
inline constexpr std::size_t kMaximumLineSize = 2'048;
inline constexpr std::size_t kReceiveCapacity = (kMaximumLineSize + 1) * 4;
inline constexpr std::size_t kOutboundCapacity = 32;
inline constexpr std::size_t kMaximumLinesPerService = 8;
inline constexpr std::size_t kMaximumIoOperationsPerService = 8;

inline constexpr char kCapabilityPing[] = "host.ping";
inline constexpr char kCapabilityWorldPhaseObserve[] = "world.phase.observe";
inline constexpr char kCapabilityPlacedContentAuthorityObserve[] = "placed-content.authority.observe";

} // namespace sunrise::server::script_host::protocol
