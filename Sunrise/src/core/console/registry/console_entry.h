#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../definition.h"

namespace sunrise::core::console::registry {

/**
 * What one entry is.
 *
 * A variable holds a value the owning module already stores, so it answers a read and accepts a
 * write. A command performs work and answers only with its result. The split is what lets the
 * console offer a value for completion without running anything.
 */
enum class Kind : std::uint8_t {
    variable,
    command,
    count,
};

/**
 * One declared argument of a command.
 *
 * An argument carries the same bounds and choices a variable does. The checks are the same ones,
 * so declaring them here means a command's arguments are validated before its handler runs,
 * exactly as a variable's value is.
 */
struct Argument {
    std::string_view name;
    std::string_view help;
    Type type{Type::boolean};
    /** Inclusive bounds a numeric argument accepts. Equal values mean the domain is unbounded. */
    double minimum{};
    double maximum{};
    /** Choices this argument accepts, or empty when any value of its type is allowed. */
    std::span<const std::string_view> choices{};
};

/**
 * Reads the value a variable currently holds.
 * @param output Filled only when the read succeeds.
 * @return True when the value was read.
 */
using ReadCallback = bool (*)(Value& output) noexcept;

/**
 * Writes a value the parser already checked against the declared type, bounds and choices.
 * @param value Checked value, of the entry's declared type.
 * @return The outcome. A handler that declines reports why rather than reporting `ok`.
 */
using WriteCallback = Status (*)(const Value& value) noexcept;

/**
 * Runs a command with arguments the parser already checked.
 * @param arguments One checked value per declared argument, in declared order.
 * @param output Result to fill. It arrives cleared.
 */
using InvokeCallback = void (*)(std::span<const Value> arguments, Result& output) noexcept;

/**
 * One registered console entry.
 *
 * Names, help and choices are borrowed rather than copied: every publisher is a module that
 * outlives the registry, and their text is already stored as constants. That keeps a
 * registration free of allocation and keeps this descriptor trivially copyable, which is what
 * lets a snapshot be taken by value under the lock.
 *
 * A variable does not carry storage. It borrows the module's own field through its callbacks, so
 * the console and that module's panel read the same memory and cannot drift apart.
 */
struct Descriptor {
    std::string_view name;
    std::string_view help;
    Kind kind{Kind::command};

    /** Declared domain of a variable's value. Unused by a command. */
    Type type{Type::boolean};
    /** Inclusive bounds a numeric variable accepts. Equal values mean the domain is unbounded. */
    double minimum{};
    double maximum{};
    /** Choices a text variable accepts, or empty when any text is allowed. */
    std::span<const std::string_view> choices{};

    ReadCallback read{};
    /** Absent on a variable that only reports. Writing one is refused rather than ignored. */
    WriteCallback write{};

    /** Arguments a command declares, in the order it reads them. */
    std::span<const Argument> arguments{};
    InvokeCallback invoke{};
};

/** @param descriptor Entry to test. @return True when a numeric variable declares real bounds. */
[[nodiscard]] constexpr bool has_bounds(const Descriptor& descriptor) noexcept {
    return descriptor.kind == Kind::variable && descriptor.minimum < descriptor.maximum
           && (descriptor.type == Type::integer || descriptor.type == Type::real);
}

/**
 * Tests whether a descriptor declares everything its kind needs.
 *
 * The registry refuses anything this rejects, so a malformed publisher fails at startup rather
 * than at the first call, when a reader would be looking at the console instead of the log.
 *
 * @param descriptor Entry to test.
 * @return True when the entry is complete.
 */
[[nodiscard]] constexpr bool is_complete(const Descriptor& descriptor) noexcept {
    if (descriptor.name.empty() || descriptor.name.size() >= kNameCapacity) {
        return false;
    }
    // Lookups fold what a reader types, which only reaches a name that is itself folded. Refusing
    // the rest here keeps that guarantee at registration instead of leaving one entry unreachable.
    if (!is_folded(descriptor.name)) {
        return false;
    }
    if (descriptor.kind >= Kind::count || descriptor.type >= Type::count) {
        return false;
    }
    if (descriptor.kind == Kind::variable) {
        // A read is required and a write is not. A write-only variable would show a value the
        // console could never print, while a read-only one is an ordinary thing to publish: what
        // a session currently is, rather than what it should be set to.
        return descriptor.read != nullptr;
    }
    if (descriptor.invoke == nullptr || descriptor.arguments.size() > kArgumentCapacity) {
        return false;
    }
    for (const Argument& argument : descriptor.arguments) {
        if (argument.name.empty() || argument.type >= Type::count) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::core::console::registry
