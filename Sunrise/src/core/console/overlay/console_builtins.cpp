#include "console_builtins.h"

#include <array>
#include <cstdint>
#include <span>

#include "../output/console_format.h"
#include "../output/console_output.h"
#include "../registry/console_registry.h"
#include "console_completion.h"

namespace sunrise::core::console::builtins {
namespace {

/** Reads a text argument as the view its handler can match on. */
[[nodiscard]] std::string_view argument_text(const Value& value) noexcept {
    return {value.text.data(), value.textLength};
}

/** Drops every scrollback line. */
void run_clear(std::span<const Value>, Result& output) noexcept {
    output::clear();
    output.status = Status::ok;
    // Nothing is written to the scrollback here: the point of the command is an empty one, and a
    // line saying so would leave it not quite empty.
}

/** Lists every entry whose name contains a run of text. */
void run_find(std::span<const Value> arguments, Result& output) noexcept {
    const std::string_view needle = argument_text(arguments[0]);
    const registry::RegistrySnapshot view = registry::snapshot();

    std::size_t matched = 0;
    for (const registry::Descriptor& entry : view.entries()) {
        // The help is searched as well as the name. A reader looking for "magazine" is reaching
        // for the entry whose help says it, and its name never will.
        if (!contains_folded(entry.name, needle) && !contains_folded(entry.help, needle)) {
            continue;
        }
        ++matched;
        std::array<char, output::kScrollbackLineCapacity> usage{};
        std::size_t usageLength = 0;
        output::format_usage(entry, usage, usageLength);
        output::write(output::LineKind::answer, {usage.data(), usageLength});
    }

    output.status = Status::ok;
    Value count{};
    count.type = Type::integer;
    count.integer = static_cast<std::int64_t>(matched);
    static_cast<void>(add_row(output, "matches", count));
    if (matched == 0) {
        set_summary(output, "Nothing carries that text.");
    }
}

/** Prints one entry's usage and what it is for. */
void run_help(std::span<const Value> arguments, Result& output) noexcept {
    const std::string_view name = argument_text(arguments[0]);
    registry::Descriptor entry{};
    if (!registry::find(name, entry)) {
        // The reader named something that does not exist, so the useful answer is what does.
        const overlay::Completion nearby = overlay::suggest(name);
        output.status = Status::unknownName;
        set_summary(output,
                    nearby.count == 0 ? "No such name."
                                      : "No such name. Did you mean one of these?");
        for (std::size_t index = 0; index < nearby.count; ++index) {
            output::write(output::LineKind::answer, nearby.matches[index]);
        }
        return;
    }

    std::array<char, output::kScrollbackLineCapacity> usage{};
    std::size_t usageLength = 0;
    output::format_usage(entry, usage, usageLength);
    output::write(output::LineKind::answer, {usage.data(), usageLength});
    output::write(output::LineKind::answer, entry.help);

    for (const registry::Argument& argument : entry.arguments) {
        std::array<char, output::kScrollbackLineCapacity> line{};
        std::size_t length = 0;
        store_text(argument.name, line, length);
        output::write(output::LineKind::answer, {line.data(), length});
        output::write(output::LineKind::answer, argument.help);
    }
    output.status = Status::ok;
}

constexpr std::array<registry::Argument, 1> kFindArguments{
    registry::Argument{.name = "text",
                       .help = "Run of text an entry name or its help must contain.",
                       .type = Type::text}};

constexpr std::array<registry::Argument, 1> kHelpArguments{registry::Argument{
    .name = "name", .help = "Exact name of the entry to describe.", .type = Type::text}};

/** @return The console's own entries, built once. */
[[nodiscard]] std::array<registry::Descriptor, 3> builtin_entries() noexcept {
    registry::Descriptor clear{};
    clear.name = "console.clear";
    clear.help = "Drops every line the console is holding.";
    clear.kind = registry::Kind::command;
    clear.invoke = &run_clear;

    registry::Descriptor find{};
    find.name = "console.find";
    find.help = "Lists every command and variable whose name or help contains a run of text.";
    find.kind = registry::Kind::command;
    find.arguments = kFindArguments;
    find.invoke = &run_find;

    registry::Descriptor help{};
    help.name = "console.help";
    help.help = "Describes one command or variable, naming close matches when it does not exist.";
    help.kind = registry::Kind::command;
    help.arguments = kHelpArguments;
    help.invoke = &run_help;

    return {clear, find, help};
}

} // namespace

/** Publishes the console's own commands. */
bool initialize() noexcept {
    const std::array<registry::Descriptor, 3> entries = builtin_entries();
    return registry::register_entries(entries) == registry::RegistrationResult::registered;
}

/** Removes the console's own commands. */
void shutdown() noexcept {
    static_cast<void>(registry::unregister_prefix(kBuiltinPrefix));
}

} // namespace sunrise::core::console::builtins
