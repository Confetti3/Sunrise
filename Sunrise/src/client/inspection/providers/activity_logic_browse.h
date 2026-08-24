#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sunrise::client::inspection::providers::activity_logic {

struct BrowseSummary final {
    std::uint32_t scenarioTag{};
    std::string activityName;
    std::string destination;
};

namespace browse {

struct View final {
    const BrowseSummary* current{};
    std::string destination;
    std::vector<const BrowseSummary*> local;
    std::vector<const BrowseSummary*> all;
};

[[nodiscard]] inline char ascii_lower(char value) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

[[nodiscard]] inline bool contains_case_insensitive(std::string_view text, std::string_view query) {
    if (query.empty()) {
        return true;
    }
    return std::search(
               text.begin(),
               text.end(),
               query.begin(),
               query.end(),
               [](char left, char right) { return ascii_lower(left) == ascii_lower(right); })
           != text.end();
}

[[nodiscard]] inline bool matches(const BrowseSummary& summary, std::string_view query) {
    if (query.empty() || contains_case_insensitive(summary.activityName, query)
        || contains_case_insensitive(summary.destination, query)) {
        return true;
    }
    std::array<char, 32> tag{};
    std::snprintf(tag.data(), tag.size(), "0x%08X", summary.scenarioTag);
    return contains_case_insensitive(tag.data(), query);
}

[[nodiscard]] inline bool display_less(const BrowseSummary* left,
                                       const BrowseSummary* right) noexcept {
    if (left->destination != right->destination) {
        return left->destination < right->destination;
    }
    if (left->activityName != right->activityName) {
        return left->activityName < right->activityName;
    }
    return left->scenarioTag < right->scenarioTag;
}

[[nodiscard]] inline View build(std::span<const BrowseSummary> entries,
                                std::uint32_t currentScenario,
                                std::string_view query = {}) {
    View view;
    for (const BrowseSummary& entry : entries) {
        if (entry.scenarioTag == currentScenario) {
            view.current = &entry;
            view.destination = entry.destination;
            break;
        }
    }

    for (const BrowseSummary& entry : entries) {
        if (view.current != nullptr && &entry != view.current
            && entry.destination == view.destination) {
            view.local.push_back(&entry);
        }
        if (matches(entry, query)) {
            view.all.push_back(&entry);
        }
    }

    std::ranges::stable_sort(view.local, display_less);
    std::ranges::stable_sort(
        view.all, [currentScenario](const BrowseSummary* left, const BrowseSummary* right) {
            const bool leftCurrent = left->scenarioTag == currentScenario;
            const bool rightCurrent = right->scenarioTag == currentScenario;
            if (leftCurrent != rightCurrent) {
                return leftCurrent;
            }
            return display_less(left, right);
        });
    return view;
}

[[nodiscard]] inline bool contains_scenario(std::span<const BrowseSummary> entries,
                                            std::uint32_t scenarioTag) noexcept {
    return std::ranges::any_of(entries, [scenarioTag](const BrowseSummary& entry) {
        return entry.scenarioTag == scenarioTag;
    });
}

} // namespace browse
} // namespace sunrise::client::inspection::providers::activity_logic
