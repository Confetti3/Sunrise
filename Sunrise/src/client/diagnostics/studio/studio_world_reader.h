#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../hooks/world_objects/dynamic_object_registry.h"
#include "../../hooks/world_objects/world_object_registry.h"

namespace sunrise::client::diagnostics::studio {

/** The native index has 64 bubble ordinals and eight slice states per bubble. */
inline constexpr std::size_t kNativeSliceCapacity = 512;

struct LoadedSlice final {
    std::uint32_t bubbleHash{};
    std::uint16_t sliceIndex{};
    std::uint8_t residency{};
};

struct WorldSnapshot final {
    bool available{};
    bool present{};
    std::array<char, 40> activityName{};
    std::uint32_t mapDefinitionTag{};
    std::uint32_t mapRootTag{};
    std::uint32_t mapNameHash{};
    std::uint32_t scenarioTag{};
    std::int32_t currentSlice{-1};
    std::uint32_t loadedCount{};
    std::array<LoadedSlice, kNativeSliceCapacity> loaded{};
};

/** Bytes of the object datum layout the reader knows. */
inline constexpr std::size_t kObjectDatumBytes = 224;

/**
 * `gone` means the handle is reused or the object is destroyed; `flagsValid` covers the flags.
 * `stateKnown` means the parent chain was read, so `placedNotHidden` holds.
 */
struct PlacementSnapshot final {
    hooks::world_objects::Instance identity{};
    std::array<float, 3> position{};
    std::array<float, 4> orientation{};
    float scale{};
    std::uint32_t parentHandle{0xFFFFFFFFU};
    std::uint32_t datumFlags{};
    std::uint32_t rootHandle{0xFFFFFFFFU};
    std::uint32_t rootFlags{};
    /** Flags of the datum and every ancestor, combined. */
    std::uint32_t chainFlags{};
    bool flagsValid{};
    bool gone{};
    bool stateKnown{};
    bool placedNotHidden{};
};

/** Datum flag bits at datum +4 that the placement test reads. */
enum DatumFlags : std::uint32_t {
    datumUnbuilt = 1,
    datumBuilding = 2,
    datumDestroyed = 4,
    datumInCells = 0x20,
    datumKeptOut = 0x80,
    datumHidden = 0x400,
    datumHiddenInEffect = 0x800,
};

/**
 * True when the object is built, its root is linked into spatial cells, and nothing hides it.
 * TODO: add active and visible; no per-object bit for either is known yet.
 */
[[nodiscard]] constexpr bool is_placed_and_not_hidden(std::uint32_t datumFlags,
                                                      std::uint32_t rootFlags,
                                                      std::uint32_t chainFlags) noexcept {
    return (datumFlags & (datumUnbuilt | datumBuilding | datumHiddenInEffect)) == 0
           && (rootFlags & datumInCells) != 0
           && (chainFlags & (datumDestroyed | datumKeptOut | datumHidden)) == 0;
}

/** Resolve once before polling; the owning diagnostic lifecycle serializes these calls. */
[[nodiscard]] bool initialize_world_reader() noexcept;
/** Clear only after the diagnostic poll has stopped. */
void clear_world_reader() noexcept;
/** Sample on the existing client frame callback; residency 3 is loaded and 4 is current. */
[[nodiscard]] WorldSnapshot read_world() noexcept;
/**
 * Copies the composed pose and uniform scale only while the placement generation remains valid.
 * @param raw Receives the datum bytes when not null.
 * @return True when the pose was read; false leaves `gone` set only for a gone object.
 */
[[nodiscard]] bool read_placement(const hooks::world_objects::Instance& identity,
                                  PlacementSnapshot& output,
                                  std::array<std::byte, kObjectDatumBytes>* raw = nullptr) noexcept;
/** Dynamic objects retain their own native generation and never inherit a package placement. */
[[nodiscard]] bool read_dynamic(const hooks::world_objects::DynamicInstance& identity,
                                PlacementSnapshot& output,
                                std::array<std::byte, kObjectDatumBytes>* raw = nullptr) noexcept;

} // namespace sunrise::client::diagnostics::studio
