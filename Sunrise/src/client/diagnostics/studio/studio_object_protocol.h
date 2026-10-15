#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::diagnostics::studio::object_wire {

/** Object snapshot requests use a separate local mapping and the ASCII magic SSOB. */
inline constexpr std::uint32_t kMagic = 0x424F5353, kVersion = 1;
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Object.%lu";
/** One answer holds at most 64 attached objects, 32 drawn models and 256 datum bytes. */
inline constexpr std::size_t kChildCapacity = 64, kModelCapacity = 32, kDatumCapacity = 256;
/** Studio reads these offsets directly. */
inline constexpr std::size_t kRequestOffset = 24, kSequenceOffset = 28, kRecordOffset = 64;
inline constexpr std::size_t kRootOffset = 148;
inline constexpr std::size_t kChildrenOffset = 160, kModelsOffset = 672, kDatumOffset = 800;
inline constexpr std::size_t kMappingBytes = kDatumOffset + kDatumCapacity;

/** absent: the handle no longer names that object. unavailable: nothing to look it up in. */
enum class Status : std::uint32_t { pending, complete, absent, unavailable };
/** placedNotHidden and the root fields are meaningful only with stateKnown. */
enum RecordFlags : std::uint32_t {
    transformValid = 1,
    dynamic = 2,
    placedNotHidden = 4,
    childrenTruncated = 8,
    poseCaptured = 16,
    datumValid = 32,
    stateKnown = 64,
};

struct Child final {
    std::uint32_t handle{}, generation{};
};

/**
 * The editor writes handle and generation, then a new request id. The game answers each id once;
 * the sequence covers the response and its record.
 */
struct alignas(8) SharedObject final {
    std::uint32_t magic{}, version{}, bytes{}, editorPid{};
    alignas(8) volatile std::int64_t heartbeat{};
    alignas(4) volatile std::int32_t request{};
    alignas(4) volatile std::int32_t sequence{};
    std::uint32_t handle{}, generation{};
    std::uint32_t response{};
    Status status{};
    std::uint32_t gamePid{}, clientVersion{};
    std::uint64_t time{};
    std::uint64_t identity{};
    std::uint32_t objectListTag{}, entryIndex{};
    std::uint32_t classTag{}, datumFlags{};
    std::uint32_t parentHandle{}, flags{};
    std::array<float, 3> position{};
    float scale{};
    std::array<float, 4> orientation{};
    std::uint32_t childCount{}, modelCount{};
    std::uint64_t poseTime{};
    std::uint32_t datumBytes{};
    std::uint32_t rootHandle{}, rootFlags{}, chainFlags{};
    std::array<Child, kChildCapacity> children{};
    std::array<std::uint32_t, kModelCapacity> models{};
    std::array<std::byte, kDatumCapacity> datum{};
};

static_assert(offsetof(SharedObject, request) == kRequestOffset);
static_assert(offsetof(SharedObject, sequence) == kSequenceOffset);
static_assert(offsetof(SharedObject, identity) == kRecordOffset);
static_assert(offsetof(SharedObject, rootHandle) == kRootOffset);
static_assert(offsetof(SharedObject, children) == kChildrenOffset);
static_assert(offsetof(SharedObject, models) == kModelsOffset);
static_assert(offsetof(SharedObject, datum) == kDatumOffset);
static_assert(sizeof(SharedObject) == kMappingBytes);

} // namespace sunrise::client::diagnostics::studio::object_wire
