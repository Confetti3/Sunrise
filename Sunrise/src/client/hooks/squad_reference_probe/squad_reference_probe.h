#pragma once

#include <cstdint>

namespace sunrise::client::hooks::squad_reference_probe {

struct RuntimeSnapshot final {
    std::uintptr_t lastActiveInstance{};
    std::uint64_t applyCalls{};
    std::uint64_t resolveCalls{};
    std::uint64_t buildRequestCalls{};
    std::uint64_t createOutcomeCalls{};
    std::uint64_t lastBuildTick{};
    std::uintptr_t lastBuildInstance{};
    std::uintptr_t lastBuildResult{};
    std::int32_t lastBuildProduced{-1};
    std::int32_t lastBuildMode{-1};
    std::int32_t lastBuildMemberCount{-1};
    std::int32_t lastBuildFirst{-1};
    std::int32_t lastBuildSecond{-1};
    std::uint32_t requestedFirst{};
    std::uint32_t requestedSecond{};
    std::uint32_t pending{};
    std::uintptr_t decodedState{};
    std::uint32_t decodedSlotCount{};
    std::uint32_t decodedRequestedFirst{};
    std::uint32_t decodedRequestedSecond{};
    std::uint32_t decodedGeneration{};
    std::uint8_t decodedMode{};
    bool decodedActive{};
};

/** Resets terminal shutdown state for a new client runtime. */
void initialize() noexcept;

/** Installs build-86657 read-only spawner, resolver, request, and create-outcome probes. */
[[nodiscard]] bool install() noexcept;

/** Retries late attachment after the protected AI code has unpacked. */
void service(std::uint64_t now) noexcept;

/** Copies bounded counters and lock-coherent last spawner, build-request, and decoded-state records. */
[[nodiscard]] RuntimeSnapshot runtime_snapshot() noexcept;

/** Detaches both probes before the client image can unload. */
[[nodiscard]] bool uninstall() noexcept;

} // namespace sunrise::client::hooks::squad_reference_probe
