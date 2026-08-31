#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../definition.h"
#include "console_entry.h"

namespace sunrise::core::console::registry {

/** Stable identity of one registration. Zero never names a live entry. */
using RegistrationId = std::uint64_t;
inline constexpr RegistrationId kNoRegistration = 0;

/**
 * Registration outcomes, kept apart so a publisher's failure names its own cause.
 *
 * A module publishes at startup, where nothing is on screen yet, so the only useful report is one
 * a log line can state plainly.
 */
enum class RegistrationResult : std::uint8_t {
    registered,
    /** The descriptor does not declare what its kind needs. See `is_complete`. */
    incompleteDescriptor,
    /** Another entry already carries that name. Names are the console's whole addressing scheme. */
    duplicateName,
    /** The fixed table is full. */
    capacityReached,
};

/**
 * A copy of the entry table, taken under one shared lock.
 *
 * Completion and help walk every entry while the console draws, and the draw happens on the
 * render thread while a module may still be publishing. Copying once per use keeps that walk off
 * the lock instead of holding it for the length of a frame.
 */
class RegistrySnapshot final {
public:
    /** @return Every registered entry, ordered by name. */
    [[nodiscard]] std::span<const Descriptor> entries() const noexcept;

    /** @return Registry revision captured with the entries. */
    [[nodiscard]] std::uint64_t revision() const noexcept;

private:
    friend RegistrySnapshot snapshot() noexcept;

    std::array<Descriptor, kEntryCapacity> entries_{};
    std::size_t count_{};
    std::uint64_t revision_{};
};

/**
 * Copies one descriptor into the fixed table, keeping the table ordered by name.
 *
 * Order is held at registration rather than at use because publishing happens once per module at
 * startup and the ordered walk happens once per drawn frame.
 *
 * @param descriptor Entry to publish. It is copied; its borrowed text must outlive the registry.
 * @return The outcome. Nothing changes when the descriptor is rejected.
 */
[[nodiscard]] RegistrationResult register_entry(const Descriptor& descriptor) noexcept;

/**
 * Publishes a whole module's entries as one mutation.
 *
 * Either every entry lands or none does: a module half-published would offer a console that
 * answers some of its own help, which is worse than a module that plainly failed.
 *
 * @param descriptors Entries to publish, in any order.
 * @return The outcome of the first entry that failed, or `registered` when all landed.
 */
[[nodiscard]] RegistrationResult register_entries(std::span<const Descriptor> descriptors) noexcept;

/**
 * Removes every entry whose name begins with one module's prefix.
 *
 * A module owns the `module.` half of its names, so its prefix is what it releases at shutdown.
 *
 * @param prefix Name prefix, conventionally `module.` including the dot.
 * @return Count removed.
 */
std::size_t unregister_prefix(std::string_view prefix) noexcept;

/**
 * Finds one entry by its exact name.
 * @param name Exact registered name.
 * @param output Filled only when an entry matched.
 * @return True when an entry matched.
 */
[[nodiscard]] bool find(std::string_view name, Descriptor& output) noexcept;

/**
 * Resolves an entry and the unique identity of this exact registration under one lock.
 * @param name Name to find, case-insensitively.
 * @param output Descriptor filled on success.
 * @param registration Filled with a nonzero identity on success.
 * @return True when the entry exists.
 */
[[nodiscard]] bool resolve(std::string_view name,
                           Descriptor& output,
                           RegistrationId& registration) noexcept;

/**
 * Tests whether a previously resolved registration still owns its name.
 * @param name Borrowed registered name.
 * @param registration Identity returned by `resolve`.
 * @return True only while that exact registration remains live.
 */
[[nodiscard]] bool is_current(std::string_view name, RegistrationId registration) noexcept;

/** @return A copy of every entry, taken under the lock, ordered by name. */
[[nodiscard]] RegistrySnapshot snapshot() noexcept;

/** Clears every registration. The revision keeps rising. */
void clear() noexcept;

/** Clears all storage and resets the revision. */
void shutdown() noexcept;

} // namespace sunrise::core::console::registry
