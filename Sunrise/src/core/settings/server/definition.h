#pragma once

#include "../../../state/entitlements/definition.h"
#include "activation/definition.h"
#include "gameplay/definition.h"

namespace sunrise::core::settings::server {

/** The loopback port the BAP listener binds, and the relay port SignOn hands the Client. */
inline constexpr std::uint16_t kDefaultBapPort = 30974;

/** The loopback port the console endpoint binds when it is enabled. */
inline constexpr std::uint16_t kDefaultConsoleEndpointPort = 30975;

/** Local-only console endpoint policy. */
struct ConsoleEndpointSettings {
    bool enabled{false};
    std::uint16_t port{kDefaultConsoleEndpointPort};
};

/** Read-only Server settings. */
struct Settings {
    /** Authored ownership policy declared by SignOn and defined by the content manifest. */
    state::entitlements::Table entitlements{};
    /** Gameplay UDP endpoint topology. Disabled leaves the channel unpublished. */
    gameplay::Settings gameplay{};
    /** Per-domain gates for the default client-activation work. */
    activation::Settings activation{};
    /** BAP port. The listener binds it and SignOn publishes it. Zero is the no-relay sentinel. */
    std::uint16_t bapPort{kDefaultBapPort};
    /** Loopback endpoint that answers console lines. Disabled leaves no socket bound. */
    ConsoleEndpointSettings consoleEndpoint{};
};

} // namespace sunrise::core::settings::server
