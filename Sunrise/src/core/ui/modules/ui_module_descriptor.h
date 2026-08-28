#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::core::ui::modules {

/** Ownership keeps Core, Client and Server pages in separate menu groups. */
enum class Owner : std::uint8_t {
    client,
    server,
    core,
};

/** 48 bytes fit a namespaced stable ID and keep every slot the same size. */
inline constexpr std::size_t kStableIdCapacity = 48;
/** 64 bytes hold one short menu label with no dynamic storage. */
inline constexpr std::size_t kDisplayNameCapacity = 64;

/** Frame entry of one statically linked UI module. It does not depend on the renderer. */
using FrameCallback = void (*)() noexcept;

/** UI module details, copied into the fixed registry and never changed after. */
class Descriptor final {
public:
    Descriptor() noexcept = default;

    /** @return The domain that owns the module. */
    [[nodiscard]] Owner owner() const noexcept;

    /** @return The stable lowercase module ID. */
    [[nodiscard]] std::string_view stable_id() const noexcept;

    /** @return The user-facing module name. */
    [[nodiscard]] std::string_view display_name() const noexcept;

    /** @return The frame entry. It is called only while its owner panel is selected. */
    [[nodiscard]] FrameCallback frame_callback() const noexcept;

private:
    friend bool create_descriptor(Owner owner,
                                  std::string_view stableId,
                                  std::string_view displayName,
                                  FrameCallback frameCallback,
                                  Descriptor& output) noexcept;
    Owner owner_{Owner::client};
    std::array<char, kStableIdCapacity> stableId_{};
    std::size_t stableIdLength_{};
    std::array<char, kDisplayNameCapacity> displayName_{};
    std::size_t displayNameLength_{};
    FrameCallback frameCallback_{};
};

/** Builds a descriptor when every field is valid and fits its fixed storage. */
[[nodiscard]] bool create_descriptor(Owner owner,
                                     std::string_view stableId,
                                     std::string_view displayName,
                                     FrameCallback frameCallback,
                                     Descriptor& output) noexcept;

} // namespace sunrise::core::ui::modules
