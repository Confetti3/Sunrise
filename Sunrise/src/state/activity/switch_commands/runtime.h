#pragma once

#include <atomic>
#include <cstdint>

namespace sunrise::state::activity::switch_commands {

enum class Status : std::uint8_t {
    empty,
    publishing,
    pending,
    executing,
    applied,
    rejected,
    cancelled,
};

enum class Operation : std::uint8_t {
    read,
    set,
};

struct Command final {
    Operation operation{Operation::read};
    std::uint64_t sequence{};
    std::uint16_t definition{};
    std::int32_t value{};
};

struct Result final {
    Operation operation{Operation::read};
    std::uint64_t sequence{};
    std::uint8_t before{};
    std::uint8_t after{};
    bool applied{};
};

inline std::atomic<Status> g_status{Status::empty};
inline std::atomic<std::uint64_t> g_nextSequence{1};
inline std::atomic<std::uint64_t> g_sequence{};
inline std::atomic<Operation> g_operation{Operation::read};
inline std::atomic<std::uint32_t> g_definition{};
inline std::atomic<std::int32_t> g_value{};
inline std::atomic<std::uint32_t> g_before{};
inline std::atomic<std::uint32_t> g_after{};

/** Publishes one command into the single bounded bridge-to-game-thread slot. */
[[nodiscard]] inline bool publish(Operation operation,
                                  std::uint16_t definition,
                                  std::int32_t value,
                                  std::uint64_t& sequence) noexcept {
    Status expected = Status::empty;
    if (!g_status.compare_exchange_strong(
            expected, Status::publishing, std::memory_order_acq_rel)) {
        return false;
    }
    sequence = g_nextSequence.fetch_add(1, std::memory_order_relaxed);
    g_operation.store(operation, std::memory_order_relaxed);
    g_definition.store(definition, std::memory_order_relaxed);
    g_value.store(value, std::memory_order_relaxed);
    g_sequence.store(sequence, std::memory_order_relaxed);
    g_status.store(Status::pending, std::memory_order_release);
    return true;
}

[[nodiscard]] inline bool publish_read(std::uint16_t definition,
                                       std::uint64_t& sequence) noexcept {
    return publish(Operation::read, definition, 0, sequence);
}

[[nodiscard]] inline bool publish_set(std::uint16_t definition,
                                      std::int32_t value,
                                      std::uint64_t& sequence) noexcept {
    return publish(Operation::set, definition, value, sequence);
}

/** Claims the pending command exactly once from the validated activity-owner update. */
[[nodiscard]] inline bool try_claim(Command& command) noexcept {
    Status expected = Status::pending;
    if (!g_status.compare_exchange_strong(
            expected, Status::executing, std::memory_order_acq_rel)) {
        return false;
    }
    command.sequence = g_sequence.load(std::memory_order_relaxed);
    command.operation = g_operation.load(std::memory_order_relaxed);
    command.definition = static_cast<std::uint16_t>(
        g_definition.load(std::memory_order_relaxed));
    command.value = g_value.load(std::memory_order_relaxed);
    return true;
}

inline void complete(const Command& command,
                     std::uint8_t before,
                     std::uint8_t after,
                     bool applied) noexcept {
    if (g_status.load(std::memory_order_acquire) != Status::executing
        || g_sequence.load(std::memory_order_relaxed) != command.sequence) {
        return;
    }
    g_before.store(before, std::memory_order_relaxed);
    g_after.store(after, std::memory_order_relaxed);
    g_status.store(applied ? Status::applied : Status::rejected, std::memory_order_release);
}

/** Copies and consumes a finished result belonging to the bridge request. */
[[nodiscard]] inline bool try_take_result(std::uint64_t sequence, Result& result) noexcept {
    const Status status = g_status.load(std::memory_order_acquire);
    if ((status != Status::applied && status != Status::rejected)
        || g_sequence.load(std::memory_order_relaxed) != sequence) {
        return false;
    }
    result.operation = g_operation.load(std::memory_order_relaxed);
    result.sequence = sequence;
    result.before = static_cast<std::uint8_t>(g_before.load(std::memory_order_relaxed));
    result.after = static_cast<std::uint8_t>(g_after.load(std::memory_order_relaxed));
    result.applied = status == Status::applied;
    g_status.store(Status::empty, std::memory_order_release);
    return true;
}

/** Cancels a command only while the game thread has not claimed it. */
[[nodiscard]] inline bool cancel(std::uint64_t sequence) noexcept {
    if (g_sequence.load(std::memory_order_acquire) != sequence) {
        return false;
    }
    Status expected = Status::pending;
    if (!g_status.compare_exchange_strong(
            expected, Status::cancelled, std::memory_order_acq_rel)) {
        return false;
    }
    g_status.store(Status::empty, std::memory_order_release);
    return true;
}

} // namespace sunrise::state::activity::switch_commands
