#include "core/console/invoke/console_invoke.h"
#include "core/console/output/console_format.h"
#include "core/console/output/console_output.h"
#include "core/console/parser/console_line_parse.h"
#include "core/console/parser/console_value_parse.h"
#include "core/console/queue/console_queue.h"
#include "core/console/registry/console_registry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {
namespace console = sunrise::core::console;
namespace invoke = console::invoke;
namespace output = console::output;
namespace parser = console::parser;
namespace queue = console::queue;
namespace registry = console::registry;

std::atomic_bool g_release{};
std::atomic_int g_active{};
std::atomic_int g_maxActive{};
std::atomic_int g_entered{};
std::atomic_int g_staleCalls{};
std::atomic_int g_observerCalls{};
std::array<char, console::kTextCapacity> g_text{};
std::size_t g_textLength{};

[[nodiscard]] bool check(bool condition, std::string_view message) noexcept {
    if (!condition) std::cerr << "console safety test failed: " << message << '\n';
    return condition;
}

void blocking_command(std::span<const console::Value>, console::Result& result) noexcept {
    const int active = g_active.fetch_add(1) + 1;
    int previous = g_maxActive.load();
    while (active > previous && !g_maxActive.compare_exchange_weak(previous, active)) {}
    g_entered.fetch_add(1);
    while (!g_release.load(std::memory_order_acquire)) std::this_thread::yield();
    g_active.fetch_sub(1);
    result.status = console::Status::ok;
}

void stale_command(std::span<const console::Value>, console::Result& result) noexcept {
    g_staleCalls.fetch_add(1);
    result.status = console::Status::ok;
}

void observer(std::uint64_t, const console::Result&) noexcept { g_observerCalls.fetch_add(1); }

[[nodiscard]] bool read_text(console::Value& value) noexcept {
    value.type = console::Type::text;
    value.text = g_text;
    value.textLength = g_textLength;
    return true;
}

[[nodiscard]] console::Status write_text(const console::Value& value) noexcept {
    g_text = value.text;
    g_textLength = value.textLength;
    return console::Status::ok;
}

[[nodiscard]] bool failed_read(console::Value&) noexcept { return false; }
[[nodiscard]] console::Status successful_write(const console::Value&) noexcept {
    return console::Status::ok;
}

const registry::Descriptor kBlock{
    .name = "safety.block", .help = "Blocks for concurrency tests.",
    .kind = registry::Kind::command, .invoke = &blocking_command};
const registry::Descriptor kStale{
    .name = "safety.stale", .help = "Must not run after unregister.",
    .kind = registry::Kind::command, .invoke = &stale_command};
const registry::Descriptor kText{
    .name = "safety.text", .help = "Round-trip text.",
    .kind = registry::Kind::variable, .type = console::Type::text,
    .read = &read_text, .write = &write_text};
const registry::Descriptor kReadbackFailure{
    .name = "safety.readback", .help = "Fails after a successful write.",
    .kind = registry::Kind::variable, .type = console::Type::integer,
    .minimum = 0, .maximum = 10, .read = &failed_read, .write = &successful_write};

[[nodiscard]] bool wait_until(const std::atomic_int& value, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (value.load(std::memory_order_acquire) < expected
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire) >= expected;
}

[[nodiscard]] bool same_double(double first, double second) noexcept {
    return first == second;
}

} // namespace

int main() {
    bool valid = true;
    registry::shutdown(); queue::shutdown(); output::shutdown();

    std::size_t emptyLength = 7;
    console::store_text("x", std::span<char>{}, emptyLength);
    valid = check(emptyLength == 0, "empty store_text span underflowed") && valid;

    registry::Descriptor invalid = kStale;
    invalid.kind = registry::Kind::count;
    valid = check(registry::register_entry(invalid)
                      == registry::RegistrationResult::incompleteDescriptor,
                  "invalid kind was registered") && valid;
    constexpr std::array<registry::Argument, 1> invalidArguments{{
        {.name = "bad", .help = "Bad type.", .type = console::Type::count}}};
    invalid = kStale; invalid.arguments = invalidArguments;
    valid = check(registry::register_entry(invalid)
                      == registry::RegistrationResult::incompleteDescriptor,
                  "invalid argument type was registered") && valid;

    console::Value parsedValue{};
    valid = check(parser::parse_value("9223372036854775807", console::Type::integer, parsedValue)
                      == console::Status::ok
                      && parsedValue.integer == (std::numeric_limits<std::int64_t>::max)(),
                  "INT64_MAX did not parse") && valid;
    valid = check(parser::parse_value("-9223372036854775808", console::Type::integer, parsedValue)
                      == console::Status::ok
                      && parsedValue.integer == (std::numeric_limits<std::int64_t>::min)(),
                  "INT64_MIN did not parse") && valid;
    valid = check(parser::parse_value("9223372036854775808", console::Type::integer, parsedValue)
                      == console::Status::badArgument,
                  "positive int64 overflow was accepted") && valid;
    console::Value exact{.type = console::Type::integer, .integer = 9007199254740993LL};
    valid = check(parser::check_bounds(exact, 0.0, 9007199254740992.0)
                      == console::Status::outOfRange,
                  "integer bounds lost precision above 2^53") && valid;

    for (const double expected : {1.23456789, 0.0000004,
                                  (std::numeric_limits<double>::max)()}) {
        console::Value real{.type = console::Type::real, .real = expected};
        std::array<char, output::kFormattedValueCapacity> printed{};
        std::size_t length = 0;
        output::format_value(real, printed, length);
        console::Value roundTrip{};
        const auto status = parser::parse_value(
            std::string_view{printed.data(), length}, console::Type::real, roundTrip);
        const bool roundTrips = length < printed.size() && status == console::Status::ok
                                && same_double(roundTrip.real, expected);
        if (!roundTrips) {
            std::cerr << "real round-trip detail: expected=" << expected << " printed="
                      << std::string_view{printed.data(), length} << " parsed="
                      << roundTrip.real << " status=" << static_cast<int>(status) << '\n';
        }
        valid = check(roundTrips, "real did not round-trip safely") && valid;
    }
    std::string huge(400, '9');
    valid = check(parser::parse_value(huge, console::Type::real, parsedValue)
                      == console::Status::outOfRange,
                  "overflowing real was accepted") && valid;

    const std::array descriptors{kText, kReadbackFailure, kStale, kBlock};
    valid = check(registry::register_entries(descriptors)
                      == registry::RegistrationResult::registered,
                  "safety descriptors failed to register") && valid;

    console::Value text{}; text.type = console::Type::text;
    constexpr std::string_view raw = "a\\b\"c d";
    console::store_text(raw, text.text, text.textLength);
    std::array<char, output::kFormattedValueCapacity> printedText{};
    std::size_t printedTextLength = 0;
    output::format_value(text, printedText, printedTextLength);
    std::string textLine = "safety.text ";
    textLine.append(printedText.data(), printedTextLength);
    auto outcome = parser::parse_line(textLine);
    console::Result result{};
    invoke::run(outcome.invocation, result);
    valid = check(outcome.status == console::Status::ok && result.status == console::Status::ok
                      && std::string_view{g_text.data(), g_textLength} == raw,
                  "quoted text did not round-trip") && valid;
    valid = check(parser::parse_line("safety.text \"a\"b").status
                      == console::Status::badArgument,
                  "adjacent quoted and unquoted text was accepted") && valid;

    outcome = parser::parse_line("safety.readback 4");
    invoke::run(outcome.invocation, result);
    valid = check(result.status == console::Status::failed && result.summaryLength != 0,
                  "failed write read-back was reported as success") && valid;

    console::Value malformedText{};
    malformedText.type = console::Type::text;
    malformedText.textLength = (std::numeric_limits<std::size_t>::max)();
    output::format_value(malformedText, printedText, printedTextLength);
    valid = check(printedTextLength < printedText.size(), "malformed text length escaped buffer")
            && valid;
    console::Result malformedResult{};
    malformedResult.rowCount = (std::numeric_limits<std::size_t>::max)();
    malformedResult.rows[0].keyLength = (std::numeric_limits<std::size_t>::max)();
    output::write_result(malformedResult);
    valid = check(output::snapshot().lines().size() <= console::kRowCapacity,
                  "malformed result count escaped row storage") && valid;
    output::shutdown();

    auto staleOutcome = parser::parse_line("safety.stale");
    valid = check(queue::submit(staleOutcome.invocation) != queue::kNoTicket,
                  "stale test submit failed") && valid;
    valid = check(registry::unregister_prefix("safety.stale") == 1, "stale entry unregister failed")
            && valid;
    console::Result staleResult{};
    auto capture = +[](std::uint64_t, const console::Result& value) noexcept {
        if (value.status != console::Status::refused) g_observerCalls.fetch_add(1000);
    };
    valid = check(queue::drain(capture) == 1 && g_staleCalls.load() == 0
                      && g_observerCalls.load() == 0,
                  "unregistered queued callback was invoked") && valid;

    // Only one drain may execute handlers at a time.
    queue::shutdown();
    g_release.store(false); g_active.store(0); g_maxActive.store(0); g_entered.store(0);
    const auto blocked = parser::parse_line("safety.block").invocation;
    static_cast<void>(queue::submit(blocked));
    static_cast<void>(queue::submit(blocked));
    std::size_t firstRan = 0, secondRan = 99;
    std::thread first([&] { firstRan = queue::drain(nullptr); });
    valid = check(wait_until(g_entered, 1), "first drain did not enter handler") && valid;
    std::thread second([&] { secondRan = queue::drain(nullptr); });
    second.join();
    g_release.store(true, std::memory_order_release);
    first.join();
    valid = check(firstRan == 2 && secondRan == 0 && g_maxActive.load() == 1,
                  "concurrent drains executed handlers together") && valid;

    // Shutdown invalidates an in-flight completion before a new observer can see it.
    queue::shutdown();
    g_release.store(false); g_entered.store(0); g_observerCalls.store(0);
    static_cast<void>(queue::submit(blocked));
    std::thread oldDrain([] { static_cast<void>(queue::drain(nullptr)); });
    valid = check(wait_until(g_entered, 1), "shutdown-race handler did not enter") && valid;
    queue::shutdown();
    queue::observe(&observer);
    g_release.store(true, std::memory_order_release);
    oldDrain.join();
    valid = check(g_observerCalls.load() == 0,
                  "pre-shutdown result reached a new observer") && valid;

    queue::shutdown(); output::shutdown(); registry::shutdown();
    if (!valid) return 1;
    std::cout << "console-safety-ok\n";
    return 0;
}
