#include <limits>

#include "../../../state/entitlements/validation.h"
#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Checks the standalone Server settings object. */
bool Parser::server_settings(server::Settings& output) noexcept {
    output = {};
    output.entitlements = state::entitlements::authored();
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    bool hasEntitlements = false;
    bool hasBapPort = false;
    bool hasGameplay = false;
    bool hasActivation = false;
    bool hasConsoleEndpoint = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "entitlements") {
            if (hasEntitlements || !entitlements(output.entitlements)) {
                return false;
            }
            hasEntitlements = true;
        } else if (key == "bap_port") {
            std::uint64_t value = 0;
            if (hasBapPort || !unsigned_integer(value) || value == 0
                || value > (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            output.bapPort = static_cast<std::uint16_t>(value);
            hasBapPort = true;
        } else if (key == "gameplay") {
            if (hasGameplay || !gameplay_settings(output.gameplay)) {
                return false;
            }
            hasGameplay = true;
        } else if (key == "activation") {
            if (hasActivation || !activation_settings(output.activation)) {
                return false;
            }
            hasActivation = true;
        } else if (key == "console_endpoint") {
            if (hasConsoleEndpoint || !console_endpoint_settings(output.consoleEndpoint)) {
                return false;
            }
            hasConsoleEndpoint = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return state::entitlements::valid(output.entitlements);
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses the console endpoint policy. */
bool Parser::console_endpoint_settings(server::ConsoleEndpointSettings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    server::ConsoleEndpointSettings candidate = output;
    bool hasEnabled = false;
    bool hasPort = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "enabled") {
            if (hasEnabled || !boolean(candidate.enabled)) {
                return false;
            }
            hasEnabled = true;
        } else if (key == "port") {
            std::uint64_t value = 0;
            if (hasPort || !unsigned_integer(value) || value == 0
                || value > (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            candidate.port = static_cast<std::uint16_t>(value);
            hasPort = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
