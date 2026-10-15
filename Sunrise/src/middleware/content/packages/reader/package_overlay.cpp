#include <algorithm>
#include <limits>
#include <span>
#include <vector>

#include "overlay.h"

namespace sunrise::middleware::content::packages::reader::overlay {
namespace {

/** Ordered by package id, then patch, so a lookup is a binary search. */
std::vector<File> g_files{};

/** @return True when the left row sorts before the right one. */
[[nodiscard]] bool before(const File& left, const File& right) noexcept {
    return left.packageId != right.packageId ? left.packageId < right.packageId
                                             : left.patchIndex < right.patchIndex;
}

/** @return True when both rows name the same generation. */
[[nodiscard]] bool same(const File& left, const File& right) noexcept {
    return left.packageId == right.packageId && left.patchIndex == right.patchIndex;
}

} // namespace

/** Two rows for one generation would make a read depend on row order, so they are refused. */
bool publish(std::span<const File> files) noexcept {
    std::vector<File> sorted{};
    try {
        sorted.assign(files.begin(), files.end());
    } catch (...) {
        return false;
    }
    std::sort(sorted.begin(), sorted.end(), &before);
    if (std::adjacent_find(sorted.begin(), sorted.end(), &same) != sorted.end()) {
        return false;
    }
    g_files.swap(sorted);
    return true;
}

void clear() noexcept {
    std::vector<File>{}.swap(g_files);
}

bool latest(std::uint16_t packageId, std::uint32_t& patchIndex) noexcept {
    const File last{.packageId = packageId,
                    .patchIndex = (std::numeric_limits<std::uint32_t>::max)()};
    const auto end = std::upper_bound(g_files.begin(), g_files.end(), last, &before);
    if (end == g_files.begin() || std::prev(end)->packageId != packageId) {
        return false;
    }
    patchIndex = std::prev(end)->patchIndex;
    return true;
}

bool find(std::uint16_t packageId, std::uint32_t patchIndex, Path& path) noexcept {
    const File probe{.packageId = packageId, .patchIndex = patchIndex};
    const auto found = std::lower_bound(g_files.begin(), g_files.end(), probe, &before);
    if (found == g_files.end() || !same(*found, probe)) {
        return false;
    }
    path = found->path;
    return true;
}

std::span<const File> files() noexcept {
    return g_files;
}

} // namespace sunrise::middleware::content::packages::reader::overlay
