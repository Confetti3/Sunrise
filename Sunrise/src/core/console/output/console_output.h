#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../definition.h"

namespace sunrise::core::console::output {

/**
 * Lines the scrollback keeps before the oldest is replaced.
 *
 * The log view keeps 128 entries of a kibibyte each. A console line is far shorter than a log
 * event and a reader scrolls back through more of them, so this trades width for depth at a
 * comparable total.
 */
inline constexpr std::size_t kScrollbackCapacity = 256;
/** Bytes one scrollback line may carry. A longer answer is truncated rather than wrapped here. */
inline constexpr std::size_t kScrollbackLineCapacity = 256;

/**
 * What one line is.
 *
 * The overlay tints by this rather than by parsing the text, so a line's meaning survives
 * whatever a handler chose to write.
 */
enum class LineKind : std::uint8_t {
    /** A line as the reader typed it, echoed so the answer below it has a question. */
    input,
    /** An answer from a handler. */
    answer,
    /** A refusal or a failure. */
    failure,
    /** Something the console itself says, such as a banner or a cleared notice. */
    notice,
};

/** One scrollback line. */
struct Line {
    std::array<char, kScrollbackLineCapacity> text{};
    std::size_t length{};
    LineKind kind{LineKind::answer};
};

/**
 * A copy of the scrollback, taken under its lock.
 *
 * The overlay walks every line while it draws, and a handler on the draining thread may write
 * during that walk. Copying once per frame keeps the draw off the lock.
 */
class Scrollback final {
public:
    /** @return Every retained line, oldest first. */
    [[nodiscard]] std::span<const Line> lines() const noexcept;

    /** @return Count of older lines replaced before this copy was taken. */
    [[nodiscard]] std::uint64_t overwritten_count() const noexcept;

private:
    friend Scrollback snapshot() noexcept;

    std::array<Line, kScrollbackCapacity> lines_{};
    std::size_t count_{};
    std::uint64_t overwritten_{};
};

/**
 * Appends one line, replacing the oldest once the ring is full.
 * @param kind What the line is.
 * @param text Line text, truncated when it does not fit.
 */
void write(LineKind kind, std::string_view text) noexcept;

/**
 * Appends a result: its summary when it has one, then one line per row.
 *
 * Rows print as `key = value`, and a value prints the way it reads back, so a reader can copy an
 * answer straight onto the next line.
 *
 * @param result Result to print.
 */
void write_result(const Result& result) noexcept;

/** @return A copy of every retained line, taken under the lock, oldest first. */
[[nodiscard]] Scrollback snapshot() noexcept;

/** Drops every line. The overwritten count keeps rising. */
void clear() noexcept;

/** Drops every line and resets the overwritten count. */
void shutdown() noexcept;

} // namespace sunrise::core::console::output
