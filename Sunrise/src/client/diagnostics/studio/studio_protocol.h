#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace sunrise::client::diagnostics::studio {

/** The local mapping uses the ASCII magic SSLV and protocol version 1. */
inline constexpr std::uint32_t kMagic = 0x564C5353;
inline constexpr std::uint32_t kVersion = 1;
/** A connection expires after three seconds without an editor heartbeat. */
inline constexpr std::uint64_t kHeartbeatTimeout = 3000;
/** Disconnected clients check for a requested connection once per second. */
inline constexpr std::uint64_t kConnectInterval = 1000;
/** The editor and frame publisher communicate only within the local Windows session. */
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Live.%lu";
/** Protocol version 1 fixes the size and byte offsets used by the editor. */
inline constexpr std::size_t kFrameBytes = 128;
inline constexpr std::size_t kSequenceOffset = 24;
inline constexpr std::size_t kFrameTimeOffset = 32;
inline constexpr std::size_t kPlayerOffset = 56;
inline constexpr std::size_t kHorizontalFovOffset = 104;
inline constexpr std::size_t kPlayerHandleOffset = 112;

enum SnapshotFlags : std::uint32_t {
    inWorld = 1,
    playerPresent = 2,
    cameraPresent = 4,
    sliceAvailable = 8,
    slicePresent = 16,
    playerHandlePresent = 32,
};

/** All fields use little-endian fixed-width values; sequence covers only the published data. */
struct alignas(8) SharedSnapshot final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t bytes{};
    std::uint32_t editorPid{};
    alignas(8) volatile std::int64_t heartbeat{};
    alignas(4) volatile std::int32_t sequence{};
    std::uint32_t reserved{};
    std::uint64_t frameTime{};
    std::uint32_t gamePid{};
    std::uint32_t flags{};
    std::int32_t currentSlice{-1};
    std::uint32_t reserved2{};
    std::array<float, 3> player{};
    std::array<float, 3> cameraPosition{};
    std::array<float, 3> cameraForward{};
    std::array<float, 3> cameraUp{};
    float horizontalFov{};
    float aspect{};
    /** Full handle of the object the local player controls; valid with playerHandlePresent. */
    std::uint32_t playerHandle{};
    std::array<std::byte, 12> padding{};
};

static_assert(sizeof(SharedSnapshot) == kFrameBytes);
static_assert(offsetof(SharedSnapshot, sequence) == kSequenceOffset);
static_assert(offsetof(SharedSnapshot, frameTime) == kFrameTimeOffset);
static_assert(offsetof(SharedSnapshot, player) == kPlayerOffset);
static_assert(offsetof(SharedSnapshot, horizontalFov) == kHorizontalFovOffset);
static_assert(offsetof(SharedSnapshot, playerHandle) == kPlayerHandleOffset);

// --- Editor mappings: Studio creates each one, the game only opens it ---

/** Maps the editor's view named by format and this process id; null when there is none. */
template <typename View>
[[nodiscard]] View* open_editor_view(const wchar_t* format, HANDLE& mapping) noexcept {
    std::array<wchar_t, 64> name{};
    if (swprintf_s(name.data(), name.size(), format, GetCurrentProcessId()) <= 0) {
        return nullptr;
    }
    mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.data());
    if (mapping == nullptr) {
        return nullptr;
    }
    auto* const view = static_cast<View*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(View)));
    if (view == nullptr) {
        CloseHandle(mapping);
        mapping = nullptr;
    }
    return view;
}

/** True when the editor wrote this exact protocol and its own process id. */
template <typename Header>
[[nodiscard]] bool editor_header_ok(const Header& header,
                                    std::uint32_t magic,
                                    std::uint32_t version,
                                    std::size_t bytes) noexcept {
    return header.magic == magic && header.version == version && header.bytes == bytes
           && header.editorPid != 0;
}

/** Unmaps the view, then closes the mapping; both end null. */
template <typename View> void close_editor_view(HANDLE& mapping, View*& view) noexcept {
    if (view != nullptr) {
        UnmapViewOfFile(view);
        view = nullptr;
    }
    if (mapping != nullptr) {
        CloseHandle(mapping);
        mapping = nullptr;
    }
}

/** False once the heartbeat is unset, in the future, or older than timeout. */
[[nodiscard]] inline bool
editor_alive(volatile std::int64_t& heartbeat, std::uint64_t now, std::uint64_t timeout) noexcept {
    const auto beat = static_cast<std::uint64_t>(InterlockedCompareExchange64(&heartbeat, 0, 0));
    return beat != 0 && beat <= now && now - beat <= timeout;
}

} // namespace sunrise::client::diagnostics::studio
