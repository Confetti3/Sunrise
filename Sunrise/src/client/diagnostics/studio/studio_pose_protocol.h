#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::diagnostics::studio::pose_wire {

/** Skinned-draw diagnostics use a separate version and bounded local mapping. */
inline constexpr std::uint32_t kMagic = 0x504C5353, kVersion = 5;
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Pose.%lu";
/** One draw uses at most 512 bones of four vectors; one capture shares 2 MiB of vectors. */
inline constexpr std::size_t kBoneCapacity = 512, kDrawCapacity = 1024, kVectorCapacity = 131072;
/** Handles name their object slot in the low 13 bits; the owner filter has one bit per slot. */
inline constexpr std::uint32_t kSlotMask = 0x1FFF, kOwnerWordBits = 32;
/** All-one handle bits mean no object is requested or observed. */
inline constexpr std::uint32_t kNoOwner = 0xFFFFFFFFU;
inline constexpr std::size_t kOwnerWords = (kSlotMask + 1) / kOwnerWordBits;
inline constexpr std::size_t kControlHeaderBytes = 64, kSnapshotHeaderBytes = 64;
inline constexpr std::size_t kControlBytes =
    kControlHeaderBytes + kOwnerWords * sizeof(std::uint32_t);
inline constexpr std::size_t kDrawBytes = 88;
using Vector = std::array<float, 4>;

/** Studio sets one owner bit per object slot it shows; other owners are not copied. */
struct alignas(8) Control final {
    std::uint32_t magic{}, version{}, bytes{}, editorPid{};
    volatile std::int64_t heartbeat{};
    std::uint32_t clientVersion{}, bridgeReady{}, enabled{};
    std::uint32_t player{kNoOwner};
    std::array<std::uint32_t, 6> reserved{};
    std::array<std::uint32_t, kOwnerWords> owners{};
};

/** One completed primary pose; its palette is `count` vectors from index `first`. */
struct alignas(8) Draw final {
    std::uint32_t owner{}, component{}, model{}, kind{};
    std::uint32_t modelBones{}, poseBones{}, first{}, count{};
    std::array<Vector, 3> fixedRows{};
    std::uint32_t ownerGeneration{}, componentGeneration{};
};

struct alignas(8) Snapshot final {
    volatile std::int32_t sequence{};
    std::uint32_t valid{};
    std::uint64_t time{};
    std::uint32_t drawCount{}, vectorCount{}, dropped{}, reserved{};
    std::uint32_t playerValid{}, player{}, playerGeneration{}, playerReserved{};
    std::array<std::byte, 16> padding{};
    std::array<Draw, kDrawCapacity> draws{};
    std::array<Vector, kVectorCapacity> palette{};
};

struct SharedPose final {
    Control control{};
    Snapshot snapshot{};
};

static_assert(offsetof(Control, owners) == kControlHeaderBytes);
static_assert(sizeof(Control) == kControlBytes);
static_assert(sizeof(Draw) == kDrawBytes);
static_assert(offsetof(Snapshot, draws) == kSnapshotHeaderBytes);
static_assert(offsetof(SharedPose, snapshot) == kControlBytes);

} // namespace sunrise::client::diagnostics::studio::pose_wire
