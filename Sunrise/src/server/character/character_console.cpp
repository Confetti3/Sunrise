#include "character_console.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <span>

#include "../../core/console/registry/console_registry.h"
#include "../../state/account/account_state.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::character::console {
namespace {

namespace entry = core::console;
namespace registry = core::console::registry;

constexpr std::array<registry::Argument, 1> kSelectArguments{
    registry::Argument{"who", "Class name or zero-based roster index.", entry::Type::text}};
constexpr std::array<std::string_view, state::kCharacterCapacity> kSoidKeys{
    "soid_0", "soid_1", "soid_2"};
constexpr std::array<std::string_view, state::kCharacterCapacity> kSoidHexKeys{
    "soid_0_hex", "soid_1_hex", "soid_2_hex"};
constexpr std::array<std::string_view, state::kCharacterCapacity> kClassKeys{
    "class_0", "class_1", "class_2"};
constexpr std::array<std::string_view, state::kCharacterCapacity> kSelectedKeys{
    "selected_0", "selected_1", "selected_2"};

[[nodiscard]] entry::Value integer_value(std::int64_t value) noexcept {
    entry::Value result{};
    result.type = entry::Type::integer;
    result.integer = value;
    return result;
}

[[nodiscard]] entry::Value boolean_value(bool value) noexcept {
    entry::Value result{};
    result.type = entry::Type::boolean;
    result.boolean = value;
    return result;
}

[[nodiscard]] entry::Value text_value(std::string_view value) noexcept {
    entry::Value result{};
    result.type = entry::Type::text;
    entry::store_text(value, result.text, result.textLength);
    return result;
}

[[nodiscard]] entry::Value soid_text(std::uint64_t value, bool hexadecimal) noexcept {
    std::array<char, 21> digits{};
    const int written = hexadecimal
                            ? std::snprintf(digits.data(), digits.size(), "%016llX",
                                            static_cast<unsigned long long>(value))
                            : std::snprintf(digits.data(), digits.size(), "%llu",
                                            static_cast<unsigned long long>(value));
    return written > 0 ? text_value({digits.data(), static_cast<std::size_t>(written)})
                       : text_value("");
}

[[nodiscard]] std::string_view class_name(state::CharacterClass value) noexcept {
    switch (value) {
    case state::CharacterClass::titan:
        return "titan";
    case state::CharacterClass::hunter:
        return "hunter";
    case state::CharacterClass::warlock:
        return "warlock";
    }
    return "";
}

[[nodiscard]] std::size_t character_count(const state::AccountState& account) noexcept {
    return (std::min)(account.characterCount, account.characters.size());
}

[[nodiscard]] std::int64_t selected_index(const state::AccountState& account) noexcept {
    for (std::size_t index = 0; index < character_count(account); ++index) {
        if (account.characters[index].selected) {
            return static_cast<std::int64_t>(index);
        }
    }
    return -1;
}

void list(std::span<const entry::Value>, entry::Result& output) noexcept {
    const state::AccountState account = state::account_snapshot();
    static_cast<void>(entry::add_row(
        output, "count", integer_value(static_cast<std::int64_t>(character_count(account)))));
    static_cast<void>(entry::add_row(output, "selected_index", integer_value(selected_index(account))));
    for (std::size_t index = 0; index < character_count(account); ++index) {
        const state::CharacterState& character = account.characters[index];
        static_cast<void>(entry::add_row(output, kSoidKeys[index], soid_text(character.soid, false)));
        static_cast<void>(
            entry::add_row(output, kSoidHexKeys[index], soid_text(character.soid, true)));
        static_cast<void>(
            entry::add_row(output, kClassKeys[index], text_value(class_name(character.characterClass))));
        static_cast<void>(
            entry::add_row(output, kSelectedKeys[index], boolean_value(character.selected)));
    }
    output.status = entry::Status::ok;
    entry::set_summary(output, "Current account character roster.");
}

[[nodiscard]] bool requested_index(std::string_view token,
                                   const state::AccountState& account,
                                   std::size_t& output) noexcept {
    std::size_t numeric = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), numeric);
    if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) {
        if (numeric < character_count(account)) {
            output = numeric;
            return true;
        }
        return false;
    }
    std::size_t matches = 0;
    for (std::size_t index = 0; index < character_count(account); ++index) {
        if (entry::equals_folded(class_name(account.characters[index].characterClass), token)) {
            output = index;
            ++matches;
        }
    }
    return matches == 1;
}

void select(std::span<const entry::Value> arguments, entry::Result& output) noexcept {
    const state::AccountState before = state::account_snapshot();
    const std::string_view token{arguments[0].text.data(), arguments[0].textLength};
    std::size_t index = 0;
    if (!requested_index(token, before, index)) {
        output.status = entry::Status::refused;
        entry::set_summary(output, "Character must name one unique class or valid roster index.");
        return;
    }
    bool changed = false;
    if (!state::set_selected_character(before.characters[index].soid, changed)) {
        output.status = entry::Status::failed;
        entry::set_summary(output, "Character selection could not be committed.");
        return;
    }
    static_cast<void>(entry::add_row(
        output, "count", integer_value(static_cast<std::int64_t>(character_count(before)))));
    static_cast<void>(entry::add_row(output, "previous_index", integer_value(selected_index(before))));
    static_cast<void>(entry::add_row(output, "index", integer_value(static_cast<std::int64_t>(index))));
    static_cast<void>(entry::add_row(output, "soid", soid_text(before.characters[index].soid, false)));
    static_cast<void>(entry::add_row(
        output, "soid_hex", soid_text(before.characters[index].soid, true)));
    static_cast<void>(entry::add_row(
        output, "class", text_value(class_name(before.characters[index].characterClass))));
    static_cast<void>(entry::add_row(output, "changed", boolean_value(changed)));
    output.status = entry::Status::ok;
    entry::set_summary(output, "Selected character for the next account snapshot.");
}

} // namespace

bool initialize() noexcept {
    registry::Descriptor roster{};
    roster.name = "character.list";
    roster.help = "Reports the account roster and current selection.";
    roster.kind = registry::Kind::command;
    roster.invoke = &list;

    registry::Descriptor pick{};
    pick.name = "character.select";
    pick.help = "Selects a character by class or roster index for subsequent account snapshots.";
    pick.kind = registry::Kind::command;
    pick.arguments = kSelectArguments;
    pick.invoke = &select;

    const std::array entries{roster, pick};
    return registry::register_entries(entries) == registry::RegistrationResult::registered;
}

void shutdown() noexcept {
    static_cast<void>(registry::unregister_prefix(kPrefix));
}

} // namespace sunrise::server::character::console
