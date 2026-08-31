#include "core/console/invoke/console_invoke.h"
#include "core/console/output/console_output.h"
#include "core/console/overlay/console_completion.h"
#include "core/console/parser/console_line_parse.h"
#include "core/console/queue/console_queue.h"
#include "core/console/registry/console_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace {

namespace console = sunrise::core::console;
namespace invoke = console::invoke;
namespace output = console::output;
namespace overlay = console::overlay;
namespace parser = console::parser;
namespace queue = console::queue;
namespace registry = console::registry;

std::int64_t g_speed = 4;
std::size_t g_commandCalls{};
std::array<std::uint64_t, queue::kQueueCapacity> g_completionTickets{};
std::array<std::uint64_t, queue::kQueueCapacity> g_observerTickets{};
std::size_t g_completionCount{};
std::size_t g_observerCount{};

[[nodiscard]] bool check(bool condition, std::string_view message) noexcept {
    if (!condition) {
        std::cerr << "console core test failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool read_speed(console::Value& outputValue) noexcept {
    outputValue.type = console::Type::integer;
    outputValue.integer = g_speed;
    return true;
}

[[nodiscard]] console::Status write_speed(const console::Value& value) noexcept {
    g_speed = value.integer;
    return console::Status::ok;
}

void add_command(std::span<const console::Value> arguments, console::Result& result) noexcept {
    ++g_commandCalls;
    if (arguments.size() != 2) {
        result.status = console::Status::failed;
        console::set_summary(result, "unexpected argument count");
        return;
    }
    console::Value sum{};
    sum.type = console::Type::integer;
    sum.integer = arguments[0].integer + arguments[1].integer;
    result.status = console::Status::ok;
    console::set_summary(result, "sum");
    static_cast<void>(console::add_row(result, "sum", sum));
}

void completion(std::uint64_t ticket, const console::Result&) noexcept {
    if (g_completionCount < g_completionTickets.size()) {
        g_completionTickets[g_completionCount] = ticket;
        ++g_completionCount;
    }
}

void observer(std::uint64_t ticket, const console::Result&) noexcept {
    if (g_observerCount < g_observerTickets.size()) {
        g_observerTickets[g_observerCount] = ticket;
        ++g_observerCount;
    }
}

constexpr std::array<registry::Argument, 2> kAddArguments{{
    {.name = "left", .help = "First term.", .type = console::Type::integer, .minimum = -10, .maximum = 10},
    {.name = "right", .help = "Second term.", .type = console::Type::integer, .minimum = -10, .maximum = 10},
}};

const registry::Descriptor kSpeed{
    .name = "test.speed",
    .help = "Bounded speed.",
    .kind = registry::Kind::variable,
    .type = console::Type::integer,
    .minimum = 1,
    .maximum = 10,
    .read = &read_speed,
    .write = &write_speed,
};

const registry::Descriptor kAdd{
    .name = "test.add",
    .help = "Adds two numbers.",
    .kind = registry::Kind::command,
    .arguments = kAddArguments,
    .invoke = &add_command,
};

[[nodiscard]] bool result_integer(const console::Result& result,
                                  std::string_view key,
                                  std::int64_t expected) noexcept {
    return result.status == console::Status::ok && result.rowCount == 1
           && std::string_view{result.rows[0].key.data(), result.rows[0].keyLength} == key
           && result.rows[0].value.type == console::Type::integer
           && result.rows[0].value.integer == expected;
}

} // namespace

int main() {
    bool valid = true;
    registry::shutdown();
    queue::shutdown();
    output::shutdown();

    registry::Descriptor invalid = kSpeed;
    invalid.name = "Test.speed";
    valid = check(registry::register_entry(invalid)
                      == registry::RegistrationResult::incompleteDescriptor,
                  "uppercase name was admitted")
            && valid;

    const std::array entries{kSpeed, kAdd};
    valid = check(registry::register_entries(entries)
                      == registry::RegistrationResult::registered,
                  "valid batch was rejected")
            && valid;
    valid = check(registry::register_entries(entries)
                      == registry::RegistrationResult::duplicateName,
                  "duplicate batch was admitted")
            && valid;

    const registry::RegistrySnapshot view = registry::snapshot();
    valid = check(view.entries().size() == 2 && view.entries()[0].name == "test.add"
                      && view.entries()[1].name == "test.speed" && view.revision() == 1,
                  "registry order or revision is wrong")
            && valid;

    parser::Outcome parsed = parser::parse_line("TEST.SPEED");
    console::Result result{};
    invoke::run(parsed.invocation, result);
    valid = check(parsed.status == console::Status::ok
                      && result_integer(result, "test.speed", 4),
                  "case-folded variable read failed")
            && valid;

    parsed = parser::parse_line("test.speed 9");
    invoke::run(parsed.invocation, result);
    valid = check(parsed.status == console::Status::ok && g_speed == 9
                      && result_integer(result, "test.speed", 9),
                  "variable write/read-back failed")
            && valid;
    valid = check(parser::parse_line("test.speed 11").status
                      == console::Status::outOfRange,
                  "variable range was not enforced")
            && valid;
    valid = check(parser::parse_line("test.speed 2 3").status
                      == console::Status::wrongArgumentCount,
                  "variable argument count was not enforced")
            && valid;

    parsed = parser::parse_line("test.add -3 8");
    invoke::run(parsed.invocation, result);
    valid = check(parsed.status == console::Status::ok && g_commandCalls == 1
                      && result_integer(result, "sum", 5),
                  "command invocation failed")
            && valid;
    valid = check(parser::parse_line("test.add 1 12").status
                      == console::Status::outOfRange,
                  "command bounds were not enforced")
            && valid;
    valid = check(parser::parse_line("missing.name").status
                      == console::Status::unknownName,
                  "unknown name was not reported")
            && valid;

    const overlay::Completion prefix = overlay::complete("TEST.");
    valid = check(prefix.count == 2 && prefix.shared == "test."
                      && prefix.matches[0] == "test.add"
                      && prefix.matches[1] == "test.speed",
                  "prefix completion failed")
            && valid;
    const overlay::Completion suggestion = overlay::suggest("SPEED");
    valid = check(suggestion.count == 1 && suggestion.matches[0] == "test.speed",
                  "substring suggestion failed")
            && valid;

    queue::observe(&observer);
    const parser::Invocation queued = parser::parse_line("test.add 1 2").invocation;
    std::array<std::uint64_t, queue::kQueueCapacity> issued{};
    for (std::size_t index = 0; index < issued.size(); ++index) {
        issued[index] = queue::submit(queued);
        valid = check(issued[index] != queue::kNoTicket, "queue refused available capacity")
                && valid;
    }
    valid = check(queue::pending() == queue::kQueueCapacity
                      && queue::submit(queued) == queue::kNoTicket,
                  "queue capacity was not enforced")
            && valid;
    valid = check(queue::drain(&completion) == queue::kQueueCapacity && queue::pending() == 0,
                  "queue did not drain completely")
            && valid;
    valid = check(g_completionCount == queue::kQueueCapacity
                      && g_observerCount == queue::kQueueCapacity,
                  "queue callbacks missed results")
            && valid;
    for (std::size_t index = 0; index < issued.size(); ++index) {
        valid = check(g_completionTickets[index] == issued[index]
                          && g_observerTickets[index] == issued[index],
                      "queue did not preserve FIFO ticket order")
                && valid;
    }

    for (std::size_t index = 0; index <= output::kScrollbackCapacity; ++index) {
        output::write(output::LineKind::answer, index == 0 ? "first" : "later");
    }
    const output::Scrollback scrollback = output::snapshot();
    valid = check(scrollback.lines().size() == output::kScrollbackCapacity
                      && scrollback.overwritten_count() == 1
                      && std::string_view{scrollback.lines().front().text.data(),
                                          scrollback.lines().front().length}
                             == "later",
                  "scrollback ring did not replace its oldest line")
            && valid;

    valid = check(registry::unregister_prefix("test.") == 2
                      && registry::snapshot().entries().empty(),
                  "registry prefix removal failed")
            && valid;
    queue::shutdown();
    output::shutdown();
    registry::shutdown();

    if (!valid) {
        return 1;
    }
    std::cout << "console-core-ok\n";
    return 0;
}
