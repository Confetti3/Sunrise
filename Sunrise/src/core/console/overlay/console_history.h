#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../parser/console_line_parse.h"

namespace sunrise::core::console::overlay {

/**
 * Lines a reader can walk back through.
 *
 * Deep enough to hold a whole session of probing without becoming a second scrollback: a reader
 * looking further back than this is reading answers, not re-running a line.
 */
inline constexpr std::size_t kHistoryCapacity = 64;

/** One remembered line. */
struct HistoryLine {
    std::array<char, parser::kLineCapacity> text{};
    std::size_t length{};
};

/**
 * Lines already submitted, newest last, with the reader's position in them.
 *
 * The position is an offset back from the line being typed rather than an index, so remembering a
 * new line does not move where the reader is standing.
 */
struct History {
    std::array<HistoryLine, kHistoryCapacity> lines{};
    std::size_t count{};
    /** Index of the oldest retained line once the ring has wrapped. */
    std::size_t head{};
    /** Steps back from the line being typed. Zero means the reader is on that line. */
    std::size_t offset{};
};

/**
 * Reads one remembered line.
 *
 * The result borrows the history's own storage, so a caller must copy it before the history is
 * written to again. The overlay does exactly that: a recalled line goes straight into the buffer
 * being edited.
 *
 * @param history History to read.
 * @param stepsBack Steps back from the newest, where one is the newest remembered line.
 * @return The line, or empty when there is nothing that far back.
 */
[[nodiscard]] constexpr std::string_view recall(const History& history,
                                                std::size_t stepsBack) noexcept {
    if (stepsBack == 0 || stepsBack > history.count) {
        return {};
    }
    const std::size_t index = (history.head + history.count - stepsBack) % kHistoryCapacity;
    return {history.lines[index].text.data(), history.lines[index].length};
}

/**
 * Remembers one submitted line and returns the reader to the line being typed.
 *
 * A line identical to the newest is not remembered twice. Re-running a line is the most common
 * thing a reader does here, and a history filled with one repeated line is a history that has
 * stopped being useful.
 *
 * @param history History to append to.
 * @param line Line as submitted. A blank line is not remembered.
 */
constexpr void remember(History& history, std::string_view line) noexcept {
    history.offset = 0;
    if (line.empty() || line == recall(history, 1)) {
        return;
    }

    const std::size_t position = (history.head + history.count) % kHistoryCapacity;
    HistoryLine& stored = history.lines[position];
    stored.length = line.size() < stored.text.size() ? line.size() : stored.text.size() - 1;
    for (std::size_t index = 0; index < stored.text.size(); ++index) {
        stored.text[index] = index < stored.length ? line[index] : '\0';
    }
    if (history.count < kHistoryCapacity) {
        ++history.count;
    } else {
        history.head = (history.head + 1) % kHistoryCapacity;
    }
}

/**
 * Steps one line further back.
 * @param history History to walk.
 * @param output Receives the line to show when the step happened.
 * @return True when the position moved.
 */
[[nodiscard]] constexpr bool step_back(History& history, std::string_view& output) noexcept {
    if (history.offset >= history.count) {
        return false;
    }
    ++history.offset;
    output = recall(history, history.offset);
    return true;
}

/**
 * Steps one line forward, ending on the empty line the reader started from.
 * @param history History to walk.
 * @param output Receives the line to show, empty once back at the start.
 * @return True when the position moved.
 */
[[nodiscard]] constexpr bool step_forward(History& history, std::string_view& output) noexcept {
    if (history.offset == 0) {
        return false;
    }
    --history.offset;
    output = recall(history, history.offset);
    return true;
}

namespace detail {

/** @return A history holding the given lines, in order, for compile-time checking. */
[[nodiscard]] constexpr History built(std::initializer_list<std::string_view> lines) noexcept {
    History history{};
    for (const std::string_view line : lines) {
        remember(history, line);
    }
    return history;
}

/**
 * Walks back a number of steps and compares what is shown there.
 *
 * The comparison happens while the history is still alive, because a recalled line borrows that
 * history's storage and returning one from here would outlive it.
 *
 * @return True when the walk ends on the expected line.
 */
[[nodiscard]] constexpr bool
walks_to(History history, std::size_t steps, std::string_view expected) noexcept {
    std::string_view shown{};
    for (std::size_t step = 0; step < steps; ++step) {
        if (!step_back(history, shown)) {
            break;
        }
    }
    return shown == expected;
}

} // namespace detail

// Walking back reaches the newest line first, which is the one a reader most often re-runs.
static_assert(detail::walks_to(detail::built({"first", "second"}), 1, "second"));
static_assert(detail::walks_to(detail::built({"first", "second"}), 2, "first"));
// Walking past the oldest holds there rather than wrapping to the newest.
static_assert(detail::walks_to(detail::built({"first", "second"}), 5, "first"));
// A repeat is not remembered twice, so one line cannot fill the ring.
static_assert(detail::built({"same", "same", "same"}).count == 1);
static_assert(detail::built({"a", "b", "a"}).count == 3);
// A blank submission leaves the history alone.
static_assert(detail::built({"a", "", "b"}).count == 2);
static_assert(detail::built({}).count == 0);
// An empty history has nothing to show, so a step back leaves the reader on their own line.
static_assert(detail::walks_to(detail::built({}), 1, ""));

} // namespace sunrise::core::console::overlay
