#include "client/movement/movement_console.h"
#include "client/movement/movement_settings_store.h"
#include "core/console/registry/console_registry.h"
#include "server/character/character_console.h"
#include "state/runtime/runtime.h"

#include <array>
#include <cassert>
#include <span>
#include <string_view>

namespace {
namespace console = sunrise::core::console;
namespace registry = console::registry;
namespace movement = sunrise::client::movement;
namespace character = sunrise::server::character;
namespace state = sunrise::state;

std::array<registry::Descriptor, 32> g_entries{};
std::size_t g_entryCount{};
movement::Settings g_movement{};
state::AccountState g_account{};

[[nodiscard]] registry::Descriptor* find(std::string_view name) noexcept {
    for (std::size_t index = 0; index < g_entryCount; ++index)
        if (g_entries[index].name == name) return &g_entries[index];
    return nullptr;
}

[[nodiscard]] const console::Row* row(const console::Result& result,
                                      std::string_view key) noexcept {
    for (std::size_t index = 0; index < result.rowCount; ++index) {
        const console::Row& candidate = result.rows[index];
        if (std::string_view(candidate.key.data(), candidate.keyLength) == key) return &candidate;
    }
    return nullptr;
}

console::Value text(std::string_view value) noexcept {
    console::Value result{};
    result.type = console::Type::text;
    console::store_text(value, result.text, result.textLength);
    return result;
}
} // namespace

namespace sunrise::core::console::registry {
RegistrationResult register_entries(std::span<const Descriptor> descriptors) noexcept {
    if (g_entryCount + descriptors.size() > g_entries.size()) return RegistrationResult::capacityReached;
    for (const Descriptor& descriptor : descriptors) g_entries[g_entryCount++] = descriptor;
    return RegistrationResult::registered;
}
std::size_t unregister_prefix(std::string_view prefix) noexcept {
    std::size_t kept = 0;
    for (std::size_t index = 0; index < g_entryCount; ++index)
        if (!g_entries[index].name.starts_with(prefix)) g_entries[kept++] = g_entries[index];
    const std::size_t removed = g_entryCount - kept;
    g_entryCount = kept;
    return removed;
}
} // namespace sunrise::core::console::registry

namespace sunrise::client::movement {
Settings get() noexcept { return g_movement; }
bool publish(const Settings& settings) noexcept { g_movement = settings; return true; }
} // namespace sunrise::client::movement

namespace sunrise::state {
AccountState account_snapshot() noexcept { return g_account; }
bool set_selected_character(std::uint64_t soid, bool& changed) noexcept {
    changed = false;
    bool found = false;
    for (std::size_t index = 0; index < g_account.characterCount; ++index) {
        const bool selected = g_account.characters[index].soid == soid;
        found = found || selected;
        changed = changed || g_account.characters[index].selected != selected;
        g_account.characters[index].selected = selected;
    }
    return found;
}
} // namespace sunrise::state

int main() {
    g_account.characterCount = 2;
    g_account.characters[0].soid = 10;
    g_account.characters[0].characterClass = state::CharacterClass::titan;
    g_account.characters[0].selected = true;
    g_account.characters[1].soid = 20;
    g_account.characters[1].characterClass = state::CharacterClass::warlock;

    assert(movement::console::initialize());
    assert(character::console::initialize());
    assert(g_entryCount == 11);

    registry::Descriptor* flySpeed = find("movement.fly_speed");
    assert(flySpeed != nullptr && flySpeed->read != nullptr && flySpeed->write != nullptr
           && flySpeed->minimum == movement::kMinimumFlySpeed
           && flySpeed->maximum == movement::kMaximumFlySpeed);
    console::Value value{};
    assert(flySpeed->read(value) && value.real == movement::kDefaultFlySpeed);
    value.type = console::Type::real; value.real = 25.0;
    assert(flySpeed->write(value) == console::Status::ok);
    assert(g_movement.flySpeed == 25.0F);

    registry::Descriptor* list = find("character.list");
    registry::Descriptor* select = find("character.select");
    assert(list != nullptr && select != nullptr && select->arguments.size() == 1);
    console::Result result{};
    list->invoke({}, result);
    assert(result.status == console::Status::ok && row(result, "count") != nullptr
           && row(result, "count")->value.integer == 2
           && row(result, "selected_index")->value.integer == 0);

    std::array<console::Value, 1> argument{text("warlock")};
    select->invoke(argument, result = {});
    assert(result.status == console::Status::ok && g_account.characters[1].selected
           && !g_account.characters[0].selected && row(result, "changed")->value.boolean);
    argument[0] = text("9");
    select->invoke(argument, result = {});
    assert(result.status == console::Status::refused);

    character::console::shutdown();
    movement::console::shutdown();
    assert(g_entryCount == 0);
}
