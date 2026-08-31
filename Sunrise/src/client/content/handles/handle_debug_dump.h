#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <span>

#include "handle_resolver.h"

namespace sunrise::client::content::handles::debug {

/** Bounded byte window logged around one resolved runtime handle. */
struct Window final {
    std::size_t bytesBefore{512};
    std::size_t bytesAfter{4096};
    std::size_t chunkBytes{32};
};

/** One bounded mapped-image range searched for definition binding slots. */
struct Range final {
    std::uintptr_t base{};
    std::size_t bytes{};
};

/** Named schema handle searched as a four-byte binding-slot identity. */
struct SchemaProbe final {
    std::uint32_t handle{};
    std::string_view label{};
};

/**
 * Resolves one content/schema handle and logs a read-only byte window around it.
 * Unreadable chunks are reported and skipped, so a page boundary does not discard the rest.
 */
[[nodiscard]] bool dump(const Source& source,
                        std::uint32_t handle,
                        std::string_view label,
                        Window window = {}) noexcept;

/**
 * Searches a mapped image once for build-bound schema slots and dumps each validated record.
 * This is the documented tagdefs slot route: handle at +0 and direct definition pointer at +8.
 */
[[nodiscard]] std::size_t dump_schema_bindings(const Source& source,
                                               Range image,
                                               std::span<const SchemaProbe> probes,
                                               Window window = {}) noexcept;

} // namespace sunrise::client::content::handles::debug
