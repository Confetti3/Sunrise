#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::diagnostics::studio::wire {

/** World diagnostics use a separate local mapping and version from camera frames. */
inline constexpr std::uint32_t kMagic = 0x574C5353;
inline constexpr std::uint32_t kVersion = 4;
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.World.%lu";
/** Native slice ordinals fit 512 rows; the placement ledger holds 16384 candidates. */
inline constexpr std::size_t kSliceCapacity = 512;
inline constexpr std::size_t kPlacementCapacity = 16384;
inline constexpr std::size_t kControlBytes = 32;
inline constexpr std::size_t kWorldHeaderBytes = 96;
inline constexpr std::size_t kSliceBytes = 8;
inline constexpr std::size_t kPlacementHeaderBytes = 64;
inline constexpr std::size_t kPlacementBytes = 80;
/** Studio reads these offsets directly. */
inline constexpr std::size_t kRowClassOffset = 24, kRowTimeOffset = 64, kExcludedOffset = 48;
inline constexpr std::size_t kWorldBytes = kWorldHeaderBytes + kSliceCapacity * kSliceBytes;
inline constexpr std::size_t kPlacementsBytes =
    kPlacementHeaderBytes + kPlacementCapacity * kPlacementBytes;
inline constexpr std::size_t kMappingBytes = kControlBytes + kWorldBytes + kPlacementsBytes;

enum WorldFlags : std::uint32_t { available = 1, present = 2 };
enum PlacementFlags : std::uint32_t { registryAvailable = 1, complete = 2, truncated = 4 };
/** placedNotHidden is meaningful only with stateKnown, when the parent chain was read. */
enum RowFlags : std::uint32_t {
    transformValid = 1,
    dynamic = 2,
    scaleValid = 4,
    placedNotHidden = 8,
    playerAttached = 16,
    stateKnown = 32,
};

struct alignas(8) Control final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t bytes{};
    std::uint32_t editorPid{};
    alignas(8) volatile std::int64_t heartbeat{};
    std::uint32_t clientVersion{};
    std::array<std::byte, 4> reserved{};
};

struct Slice final {
    std::uint32_t bubbleHash{};
    std::uint16_t index{};
    std::uint8_t residency{};
    std::uint8_t reserved{};
};

/** World identity and loaded slices publish under one independent sequence. */
struct alignas(8) World final {
    alignas(4) volatile std::int32_t sequence{};
    std::uint32_t flags{};
    std::uint64_t time{};
    std::uint32_t mapDefinitionTag{};
    std::uint32_t mapNameHash{};
    std::int32_t currentSlice{-1};
    std::uint32_t count{};
    std::array<char, 40> activity{};
    std::uint32_t scenarioTag{};
    std::uint32_t mapRootTag{};
    std::array<std::byte, 16> reserved{};
    std::array<Slice, kSliceCapacity> slices{};
};

/** A row keeps its observation time and has no parent handle; the object link answers that. */
struct alignas(8) Placement final {
    std::uint64_t identity{};
    std::uint32_t objectListTag{};
    std::uint32_t entryIndex{};
    std::uint32_t handle{};
    std::uint32_t generation{};
    std::uint32_t classTag{};
    std::uint32_t datumFlags{};
    std::array<float, 3> position{};
    float scale{};
    std::array<float, 4> orientation{};
    std::uint64_t time{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/**
 * Only complete, untruncated publications may authorize removal of prior objects. Rows of
 * destroyed or reused objects are left out and counted in `excluded`.
 */
struct alignas(8) Placements final {
    alignas(4) volatile std::int32_t sequence{};
    std::uint32_t flags{};
    std::uint64_t time{};
    std::uint32_t count{};
    std::uint32_t total{};
    std::uint64_t overflow{};
    std::uint64_t epoch{};
    std::uint64_t started{};
    std::uint32_t excluded{};
    std::array<std::byte, 12> reserved{};
    std::array<Placement, kPlacementCapacity> rows{};
};

struct SharedWorld final {
    Control control{};
    World world{};
    Placements placements{};
};

static_assert(sizeof(Control) == kControlBytes);
static_assert(sizeof(Slice) == kSliceBytes);
static_assert(offsetof(World, slices) == kWorldHeaderBytes);
static_assert(sizeof(World) == kWorldBytes);
static_assert(sizeof(Placement) == kPlacementBytes);
static_assert(offsetof(Placement, classTag) == kRowClassOffset);
static_assert(offsetof(Placement, time) == kRowTimeOffset);
static_assert(offsetof(Placements, excluded) == kExcludedOffset);
static_assert(offsetof(Placements, rows) == kPlacementHeaderBytes);
static_assert(sizeof(Placements) == kPlacementsBytes);
static_assert(sizeof(SharedWorld) == kMappingBytes);

} // namespace sunrise::client::diagnostics::studio::wire
