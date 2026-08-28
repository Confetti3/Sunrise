#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "../ui_module_descriptor.h"

namespace sunrise::core::ui::modules::registry {

/** 32 slots cover every UI domain and cap the static registry storage. */
inline constexpr std::size_t kModuleCapacity = 32;

/** A copy of the module list, taken under one shared registry lock. */
class RegistrySnapshot final {
public:
    /** @return Every descriptor, in menu order. */
    [[nodiscard]] std::span<const Descriptor> entries() const noexcept;

private:
    friend RegistrySnapshot snapshot() noexcept;

    std::array<Descriptor, kModuleCapacity> entries_{};
    std::size_t count_{};
};

/** @return Registered modules in Client, Server, Core menu order. */
[[nodiscard]] RegistrySnapshot snapshot() noexcept;

/** Owns one registry slot and makes repeated acquire/release calls idempotent. */
class PageRegistration final {
public:
    /** Registers the page once and optionally resets its state first. */
    [[nodiscard]] bool acquire(Owner owner,
                               std::string_view stableId,
                               std::string_view displayName,
                               FrameCallback callback,
                               void (*prepare)() noexcept = nullptr) noexcept;

    /** Removes the page and optionally resets its state afterwards. */
    void release(void (*finish)() noexcept = nullptr) noexcept;

private:
    // The implementation lock serializes the few startup and shutdown mutations.
    bool registered_{};
    std::string_view stableId_{};
};

/** Clears all registry storage. */
void shutdown() noexcept;

} // namespace sunrise::core::ui::modules::registry
