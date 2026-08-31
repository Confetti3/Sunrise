#include "movement_console.h"

#include <array>

#include "../../core/console/registry/console_registry.h"
#include "movement_settings_store.h"

namespace sunrise::client::movement::console {
namespace {

namespace entry = core::console;
namespace registry = core::console::registry;

/**
 * Highest Windows virtual-key code a binding may carry. Zero means no key, and 0xFF is reserved,
 * which is what the settings layer already accepts.
 */
constexpr double kHighestVirtualKey = 254.0;

/**
 * Reads one member of the settings this module owns.
 *
 * The member is a template argument because a console callback carries no context of its own.
 * Writing one function per setting by hand would be the same three lines nine times over.
 */
template <auto Member, entry::Type Domain>
[[nodiscard]] bool read_member(entry::Value& output) noexcept {
    const Settings settings = get();
    output = entry::Value{};
    output.type = Domain;
    if constexpr (Domain == entry::Type::boolean) {
        output.boolean = settings.*Member;
    } else if constexpr (Domain == entry::Type::real) {
        output.real = static_cast<double>(settings.*Member);
    } else {
        output.integer = static_cast<std::int64_t>(settings.*Member);
    }
    return true;
}

/**
 * Writes one member of the settings this module owns.
 *
 * This reads, changes one member, and publishes the whole set back, which is the only interface
 * the module offers. Nothing can interleave with it: console handlers and the pages both run on
 * the thread that draws.
 */
template <auto Member, entry::Type Domain>
[[nodiscard]] entry::Status write_member(const entry::Value& value) noexcept {
    Settings settings = get();
    if constexpr (Domain == entry::Type::boolean) {
        settings.*Member = value.boolean;
    } else if constexpr (Domain == entry::Type::real) {
        settings.*Member = static_cast<float>(value.real);
    } else {
        settings.*Member = static_cast<std::uint32_t>(value.integer);
    }
    return publish(settings) ? entry::Status::ok : entry::Status::failed;
}

/** Builds one setting's descriptor, with the bounds this module already declares. */
template <auto Member, entry::Type Domain>
[[nodiscard]] registry::Descriptor member_entry(std::string_view name,
                                                std::string_view help,
                                                double minimum = 0.0,
                                                double maximum = 0.0) noexcept {
    registry::Descriptor descriptor{};
    descriptor.name = name;
    descriptor.help = help;
    descriptor.kind = registry::Kind::variable;
    descriptor.type = Domain;
    descriptor.minimum = minimum;
    descriptor.maximum = maximum;
    descriptor.read = &read_member<Member, Domain>;
    descriptor.write = &write_member<Member, Domain>;
    return descriptor;
}

} // namespace

/** Publishes the movement settings as console variables. */
bool initialize() noexcept {
    constexpr entry::Type flag = entry::Type::boolean;
    constexpr entry::Type real = entry::Type::real;
    constexpr entry::Type key = entry::Type::integer;

    const std::array entries{
        member_entry<&Settings::enabled, flag>("movement.enabled",
                                               "Whether the teleport feature answers its key."),
        member_entry<&Settings::distance, real>(
            "movement.distance",
            "Teleport distance in world units along the camera's forward vector.",
            static_cast<double>(kMinimumDistance),
            static_cast<double>(kMaximumDistance)),
        member_entry<&Settings::virtualKey, key>(
            "movement.key",
            "Windows virtual-key code that teleports, or 0 for none.",
            0.0,
            kHighestVirtualKey),
        member_entry<&Settings::noclipEnabled, flag>("movement.noclip",
                                                     "Whether noclip answers its key."),
        member_entry<&Settings::noclipToggleKey, key>(
            "movement.noclip_key",
            "Windows virtual-key code that toggles noclip, or 0 for none.",
            0.0,
            kHighestVirtualKey),
        member_entry<&Settings::swordSkateEnabled, flag>("movement.sword_skate",
                                                         "Whether sword skating is active."),
        member_entry<&Settings::flyEnabled, flag>("movement.fly",
                                                  "Whether flight answers its key."),
        member_entry<&Settings::flyToggleKey, key>(
            "movement.fly_key",
            "Windows virtual-key code that toggles flight, or 0 for none.",
            0.0,
            kHighestVirtualKey),
        member_entry<&Settings::flySpeed, real>("movement.fly_speed",
                                                "Flight speed in world units per second.",
                                                static_cast<double>(kMinimumFlySpeed),
                                                static_cast<double>(kMaximumFlySpeed)),
    };
    return registry::register_entries(entries) == registry::RegistrationResult::registered;
}

/** Removes the movement entries. */
void shutdown() noexcept {
    static_cast<void>(registry::unregister_prefix(kPrefix));
}

} // namespace sunrise::client::movement::console
