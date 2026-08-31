#include "log_console.h"

#include <array>

#include "../../console/registry/console_registry.h"
#include "../log.h"

namespace sunrise::core::log::console {
namespace {

namespace entry = core::console;
namespace registry = core::console::registry;

/** Every threshold a channel may carry, in the order help lists them. */
constexpr std::array<std::string_view, 5> kLevelChoices{"error", "warn", "info", "debug", "off"};

/**
 * Reads one channel's threshold.
 *
 * The channel is a template argument because a console callback carries no context of its own,
 * and one function per channel written out by hand would be the same body five times.
 */
template <Channel C> [[nodiscard]] bool read_level(entry::Value& output) noexcept {
    Level current{};
    if (!level_of(C, current)) {
        return false;
    }
    output = entry::Value{};
    output.type = entry::Type::text;
    entry::store_text(level_name(current), output.text, output.textLength);
    return true;
}

/** Writes one channel's threshold. The parser has already checked the word against the choices. */
template <Channel C> [[nodiscard]] entry::Status write_level(const entry::Value& value) noexcept {
    Level level{};
    if (!level_from_name({value.text.data(), value.textLength}, level)) {
        return entry::Status::badArgument;
    }
    return set_level(C, level) ? entry::Status::ok : entry::Status::failed;
}

/** Builds one channel's descriptor. */
template <Channel C>
[[nodiscard]] registry::Descriptor level_entry(std::string_view name,
                                               std::string_view help) noexcept {
    registry::Descriptor descriptor{};
    descriptor.name = name;
    descriptor.help = help;
    descriptor.kind = registry::Kind::variable;
    descriptor.type = entry::Type::text;
    descriptor.choices = kLevelChoices;
    descriptor.read = &read_level<C>;
    descriptor.write = &write_level<C>;
    return descriptor;
}

} // namespace

/** Publishes one console variable per logging channel. */
bool initialize() noexcept {
    const std::array entries{
        level_entry<Channel::core>("log.core", "Lowest severity the Core channel records."),
        level_entry<Channel::client>("log.client", "Lowest severity the Client channel records."),
        level_entry<Channel::state>("log.state", "Lowest severity the State channel records."),
        level_entry<Channel::server>("log.server", "Lowest severity the Server channel records."),
        level_entry<Channel::middleware>("log.middleware",
                                         "Lowest severity the Middleware channel records."),
    };
    return registry::register_entries(entries) == registry::RegistrationResult::registered;
}

/** Removes the logging entries. */
void shutdown() noexcept {
    static_cast<void>(registry::unregister_prefix(kPrefix));
}

} // namespace sunrise::core::log::console
