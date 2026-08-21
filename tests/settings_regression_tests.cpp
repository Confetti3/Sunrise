#include <Windows.h>

#include <crtdbg.h>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "core/settings/settings.h"
#include "state/account/inventory/item_state.h"

namespace sunrise::core::log {

Settings defaults() noexcept {
    return {};
}

void early(std::string_view) noexcept {}

void write(Channel, Level, std::string_view) noexcept {}

} // namespace sunrise::core::log

namespace sunrise::state::activity::defaults {

ActivityDefaults authored() noexcept {
    return {};
}

bool valid(const DefaultDestination&) noexcept {
    return true;
}

bool valid(const ActivityDefaults&) noexcept {
    return true;
}

} // namespace sunrise::state::activity::defaults

namespace sunrise::state::entitlements {

Table authored() noexcept {
    return {};
}

bool valid(const Entitlement&) noexcept {
    return true;
}

bool valid(const Table&) noexcept {
    return true;
}

} // namespace sunrise::state::entitlements

namespace sunrise::state::account {

bool valid_authored(const AccountState&) noexcept {
    return true;
}

} // namespace sunrise::state::account

namespace sunrise::state::account::settings {

bool valid(const AccountSettings&) noexcept {
    return true;
}

} // namespace sunrise::state::account::settings

int failures{};

namespace {

void expect(bool value, const char* label) noexcept {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

bool parse_catalyst_policy(std::string_view json, bool& output) noexcept {
    sunrise::core::settings::Settings settings{};
    if (!sunrise::core::settings::parse(json, settings)) {
        return false;
    }
    output = settings.completeExoticCatalysts;
    return true;
}

void test_item_state_contract() noexcept {
    for (std::uint32_t flags = 0; flags <= 0x7U; ++flags) {
        expect(sunrise::state::account::inventory::valid_item_state(flags),
               "known item-state bits are valid");
    }
    expect(!sunrise::state::account::inventory::valid_item_state(0x8U),
           "unknown item-state bits are invalid");
}

void test_optional_catalyst_policy() noexcept {
    bool enabled = false;
    expect(sunrise::core::settings::kSettingsVersion == 8,
           "optional catalyst key keeps settings version 8");
    expect(parse_catalyst_policy(R"({"version":8})", enabled) && enabled,
           "version 8 without catalyst key keeps the true default");
    expect(parse_catalyst_policy(R"({"version":8,"complete_exotic_catalysts":false})", enabled)
               && !enabled,
           "version 8 accepts an explicit false catalyst policy");
    expect(
        !parse_catalyst_policy(
            R"({"version":8,"complete_exotic_catalysts":true,"complete_exotic_catalysts":false})",
            enabled),
        "duplicate catalyst policy is rejected");
    expect(!parse_catalyst_policy(R"({"version":8,"complete_exotic_catalysts":1})", enabled),
           "non-boolean catalyst policy is rejected");
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
    test_item_state_contract();
    test_optional_catalyst_policy();
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
