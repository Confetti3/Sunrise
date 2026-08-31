#include "console_completion.h"

#include "../registry/console_registry.h"

namespace sunrise::core::console::overlay {
namespace {

/** @return The longest run both names begin with. */
[[nodiscard]] std::string_view common_run(std::string_view first,
                                          std::string_view second) noexcept {
    const std::size_t shortest = first.size() < second.size() ? first.size() : second.size();
    std::size_t shared = 0;
    while (shared < shortest && first[shared] == second[shared]) {
        ++shared;
    }
    return first.substr(0, shared);
}

} // namespace

/** Finds every entry name beginning with one prefix. */
Completion complete(std::string_view prefix) noexcept {
    Completion found{};
    const registry::RegistrySnapshot view = registry::snapshot();

    bool seenAny = false;
    for (const registry::Descriptor& entry : view.entries()) {
        if (!starts_with_folded(entry.name, prefix)) {
            continue;
        }
        // The shared run is narrowed by every match, including the ones past capacity. Narrowing
        // it only over the ones that fit would let it claim a run some matching entry does not
        // have, and completing to it would then write a name nothing carries.
        found.shared = seenAny ? common_run(found.shared, entry.name) : entry.name;
        seenAny = true;

        if (found.count < kCompletionCapacity) {
            found.matches[found.count] = entry.name;
            ++found.count;
        } else {
            found.truncated = true;
        }
    }
    return found;
}

/** Finds every entry name containing one run of text. */
Completion suggest(std::string_view needle) noexcept {
    Completion found{};
    const registry::RegistrySnapshot view = registry::snapshot();

    for (const registry::Descriptor& entry : view.entries()) {
        if (!contains_folded(entry.name, needle)) {
            continue;
        }
        if (found.count >= kCompletionCapacity) {
            found.truncated = true;
            break;
        }
        found.matches[found.count] = entry.name;
        ++found.count;
    }
    return found;
}

} // namespace sunrise::core::console::overlay
