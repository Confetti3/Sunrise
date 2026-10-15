#pragma once

#include <cstdint>
#include <span>

#include "reader.h"

namespace sunrise::middleware::content::packages::reader::overlay {

/** One package generation a mod serves: a patch of an installed package, or a new package's. */
struct File final {
    std::uint16_t packageId{};
    std::uint32_t patchIndex{};
    /** Absolute path of the served file. */
    Path path{};
};

/**
 * Publishes the mod-served generations. Runs once at boot before any reader starts, so readers
 * read the table without a lock.
 * @param files One row per served file.
 * @return False when two rows name one generation or storage failed; nothing is published then.
 */
[[nodiscard]] bool publish(std::span<const File> files) noexcept;

/** Drops the table. Runs only when no reader runs. */
void clear() noexcept;

/** @return True with the highest generation a mod serves for this package. */
[[nodiscard]] bool latest(std::uint16_t packageId, std::uint32_t& patchIndex) noexcept;

/** @return True with the path of one mod-served generation. */
[[nodiscard]] bool find(std::uint16_t packageId, std::uint32_t patchIndex, Path& path) noexcept;

/** @return Every served file, ordered by package id, then patch. */
[[nodiscard]] std::span<const File> files() noexcept;

} // namespace sunrise::middleware::content::packages::reader::overlay
