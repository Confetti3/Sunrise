#include "console_output.h"

#include <Windows.h>

#include <algorithm>

#include "console_format.h"

namespace sunrise::core::console::output {
namespace {

std::array<Line, kScrollbackCapacity> g_lines{};
/** Index of the oldest retained line once the ring has wrapped. */
std::size_t g_head{};
std::size_t g_count{};
std::uint64_t g_overwritten{};
SRWLOCK g_outputLock{SRWLOCK_INIT};

} // namespace

/** @return Every retained line, oldest first. */
std::span<const Line> Scrollback::lines() const noexcept {
    return {lines_.data(), count_};
}

/** @return Count of older lines replaced before this copy was taken. */
std::uint64_t Scrollback::overwritten_count() const noexcept {
    return overwritten_;
}

/** Appends one line, replacing the oldest once the ring is full. */
void write(LineKind kind, std::string_view text) noexcept {
    AcquireSRWLockExclusive(&g_outputLock);
    const std::size_t position = (g_head + g_count) % kScrollbackCapacity;
    Line& line = g_lines[position];
    store_text(text, line.text, line.length);
    line.kind = kind;
    if (g_count < kScrollbackCapacity) {
        ++g_count;
    } else {
        // The ring is full, so this write consumed the oldest line and the window slides.
        g_head = (g_head + 1) % kScrollbackCapacity;
        ++g_overwritten;
    }
    ReleaseSRWLockExclusive(&g_outputLock);
}

/** Appends a result: its summary when it has one, then one line per row. */
void write_result(const Result& result) noexcept {
    const bool failed = result.status != Status::ok;
    if (result.summaryLength != 0) {
        write(failed ? LineKind::failure : LineKind::answer,
              std::string_view{result.summary.data(), result.summaryLength});
    } else if (failed) {
        write(LineKind::failure, status_text(result.status));
    }

    const std::size_t safeRowCount = (std::min)(result.rowCount, result.rows.size());
    for (std::size_t index = 0; index < safeRowCount; ++index) {
        const Row& row = result.rows[index];
        std::array<char, kFormattedValueCapacity> printed{};
        std::size_t printedLength = 0;
        format_value(row.value, printed, printedLength);

        std::array<char, kScrollbackLineCapacity> composed{};
        std::size_t composedLength = 0;
        const std::size_t safeKeyLength = (std::min)(row.keyLength, row.key.size());
        store_text(std::string_view{row.key.data(), safeKeyLength}, composed, composedLength);
        const std::string_view separator{" = "};
        for (const char character : separator) {
            if (composedLength + 1 < composed.size()) {
                composed[composedLength] = character;
                ++composedLength;
            }
        }
        for (std::size_t at = 0; at < printedLength; ++at) {
            if (composedLength + 1 < composed.size()) {
                composed[composedLength] = printed[at];
                ++composedLength;
            }
        }
        write(failed ? LineKind::failure : LineKind::answer,
              std::string_view{composed.data(), composedLength});
    }
}

/** @return A copy of every retained line, taken under the lock, oldest first. */
Scrollback snapshot() noexcept {
    Scrollback copy{};
    AcquireSRWLockShared(&g_outputLock);
    for (std::size_t index = 0; index < g_count; ++index) {
        copy.lines_[index] = g_lines[(g_head + index) % kScrollbackCapacity];
    }
    copy.count_ = g_count;
    copy.overwritten_ = g_overwritten;
    ReleaseSRWLockShared(&g_outputLock);
    return copy;
}

/** Drops every line. The overwritten count keeps rising. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_outputLock);
    g_overwritten += g_count;
    g_lines = {};
    g_head = 0;
    g_count = 0;
    ReleaseSRWLockExclusive(&g_outputLock);
}

/** Drops every line and resets the overwritten count. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_outputLock);
    g_lines = {};
    g_head = 0;
    g_count = 0;
    g_overwritten = 0;
    ReleaseSRWLockExclusive(&g_outputLock);
}

} // namespace sunrise::core::console::output
