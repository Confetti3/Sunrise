#include "server_runtime.h"

#include "../../client/network/consumer.h"
#include "../../core/logging/log.h"
#include "../bap/runtime.h"
#include "../character/character_console.h"
#include "../console_endpoint/console_endpoint.h"
#include "../gameplay/gameplay_runtime.h"
#include "../gameplay/mission/mission_console.h"
#include "../http/server_http.h"
#include "../transport/bap_listener.h"
#include "../ui/runtime/server_ui_module_runtime.h"

namespace sunrise::server {

/** Registers Server consumers with the Client networking boundary. */
bool initialize() noexcept {
    if (!character::console::initialize()) {
        return false;
    }
    if (!client::network::register_http_consumer(&http::consume)) {
        character::console::shutdown();
        return false;
    }
    if (client::network::register_bap_consumer(&bap::consume)) {
        // HTTP and UI remain useful when the local BAP port is already owned.
        if (!transport::initialize()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=transport stage=listen result=fail");
        }
        // The gameplay endpoint must bind before any descriptor advertises it.
        if (!gameplay::initialize()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=gameplay stage=init result=fail");
        }
        if (!gameplay::mission::console::initialize()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=mission_console stage=register result=fail");
        }
        // Disabled is a successful no-op. A configured bind failure is reported but does not
        // disable the offline server's unrelated services.
        if (!console_endpoint::initialize()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=console_endpoint stage=listen result=fail");
        }
        if (ui::runtime::initialize()) {
            return true;
        }
        console_endpoint::shutdown();
        gameplay::mission::console::shutdown();
        gameplay::shutdown();
        transport::shutdown();
        client::network::unregister_bap_consumer(&bap::consume);
    }
    // BAP registration failure rolls back the earlier HTTP registration.
    client::network::unregister_http_consumer(&http::consume);
    character::console::shutdown();
    return false;
}

/** Runs one bounded server service slice. @param now Monotonic tick count. */
void service(std::uint64_t now) noexcept {
    transport::service(now);
    gameplay::service(now);
    console_endpoint::service(now);
}

/** Unregisters Server consumers in reverse registration order. */
void shutdown() noexcept {
    ui::runtime::shutdown();
    console_endpoint::shutdown();
    gameplay::mission::console::shutdown();
    gameplay::shutdown();
    transport::shutdown();
    client::network::unregister_bap_consumer(&bap::consume);
    client::network::unregister_http_consumer(&http::consume);
    bap::shutdown();
    character::console::shutdown();
}

} // namespace sunrise::server
