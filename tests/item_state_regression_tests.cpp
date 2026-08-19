#include <Windows.h>

#include <crtdbg.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "core/settings/parser.h"
#include "core/settings/settings.h"

namespace sunrise::core::settings::parser {

struct ParserTestAccess {
    static bool equipment_item(Parser& parser,
                               state::account::inventory::Item& item) noexcept {
        return parser.equipment_item(item);
    }

    static bool at_end(Parser& parser) noexcept { return parser.at_end(); }
};

} // namespace sunrise::core::settings::parser

namespace sunrise::core::log {

Settings defaults() noexcept { return {}; }

void early(std::string_view) noexcept {}

void write(Channel, Level, std::string_view) noexcept {}

} // namespace sunrise::core::log

namespace sunrise::state::activity::defaults {

ActivityDefaults authored() noexcept { return {}; }

bool valid(const DefaultDestination&) noexcept { return true; }

bool valid(const ActivityDefaults&) noexcept { return true; }

} // namespace sunrise::state::activity::defaults

namespace sunrise::state::entitlements {

Table authored() noexcept { return {}; }

bool valid(const Entitlement&) noexcept { return true; }

bool valid(const Table&) noexcept { return true; }

} // namespace sunrise::state::entitlements

namespace sunrise::state::account {

bool valid_authored(const AccountState&) noexcept { return true; }

} // namespace sunrise::state::account

namespace sunrise::state::account::settings {

bool valid(const AccountSettings&) noexcept { return true; }

} // namespace sunrise::state::account::settings

int failures{};

namespace {

void expect(bool value, const char* label) noexcept {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

bool parse_item_flags(std::uint32_t flags,
                      sunrise::state::account::inventory::Item& item) noexcept {
    char json[192]{};
    const int written = std::snprintf(
        json,
        sizeof json,
        R"({"instance_soid":"0x1","definition_hash":"0x12345678",)"
        R"("level":50,"quantity":1,"plugs":[],"flags":%u})",
        flags);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof json) {
        return false;
    }

    sunrise::core::settings::parser::Parser parser(
        std::string_view{json, static_cast<std::size_t>(written)});
    return sunrise::core::settings::parser::ParserTestAccess::equipment_item(parser, item)
           && sunrise::core::settings::parser::ParserTestAccess::at_end(parser);
}

void test_catalyst_item_state_flags() noexcept {
    for (std::uint32_t flags = 0; flags <= 0x7U; ++flags) {
        sunrise::state::account::inventory::Item item{};
        expect(parse_item_flags(flags, item), "parser accepts known item-state bits");
        expect(item.flags == flags, "parser preserves known item-state bits");
    }

    sunrise::state::account::inventory::Item item{};
    expect(!parse_item_flags(0x8U, item), "parser rejects unknown item-state bits");
}

bool parse_catalyst_policy(std::string_view json, bool& output) noexcept {
    sunrise::core::settings::Settings settings{};
    if (!sunrise::core::settings::parse(json, settings)) {
        return false;
    }
    output = settings.completeExoticCatalysts;
    return true;
}

void test_catalyst_policy_setting() noexcept {
    bool enabled = true;
    expect(parse_catalyst_policy(R"({"version":8,"complete_exotic_catalysts":false})", enabled)
               && !enabled,
           "global catalyst policy parses false");
    expect(parse_catalyst_policy(R"({"version":8})", enabled) && enabled,
           "missing global catalyst policy keeps its default");
    expect(!parse_catalyst_policy(
               R"({"complete_exotic_catalysts":true,"complete_exotic_catalysts":false})",
               enabled),
           "duplicate global catalyst policy is rejected");
    expect(!parse_catalyst_policy(R"({"complete_exotic_catalysts":1})", enabled),
           "non-boolean global catalyst policy is rejected");
}

} // namespace

void test_exotic_catalysts() noexcept;
void test_resolved_catalyst_output() noexcept;
void test_opcode406_item_state() noexcept;

int main() {
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    test_catalyst_item_state_flags();
    test_catalyst_policy_setting();
    test_opcode406_item_state();
    test_exotic_catalysts();
    test_resolved_catalyst_output();
    if (failures != 0) {
        std::fprintf(stderr, "%d regression test(s) failed\n", failures);
        return 1;
    }
    std::puts("All regression tests passed");
    return 0;
}
