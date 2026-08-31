#include <WinSock2.h>

#include "core/console/queue/console_queue.h"
#include "core/console/registry/console_registry.h"
#include "core/logging/log.h"
#include "core/settings/settings.h"
#include "server/console_endpoint/console_endpoint.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace console = sunrise::core::console;
namespace endpoint = sunrise::server::console_endpoint;
namespace queue = console::queue;
namespace registry = console::registry;

namespace {
sunrise::core::settings::Settings g_settings{};

void ping(std::span<const console::Value>, console::Result& result) noexcept {
    result.status = console::Status::ok;
    console::set_summary(result, "pong");
}

[[nodiscard]] std::uint16_t free_loopback_port() {
    WSADATA data{};
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    const SOCKET socketValue = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(socketValue != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(socketValue, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
    int length = sizeof address;
    assert(getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    const std::uint16_t port = ntohs(address.sin_port);
    closesocket(socketValue);
    WSACleanup();
    return port;
}

[[nodiscard]] std::string receive_available(SOCKET peer) {
    std::string result;
    std::array<char, 4096> buffer{};
    for (;;) {
        const int read = recv(peer, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (read > 0) {
            result.append(buffer.data(), static_cast<std::size_t>(read));
            continue;
        }
        if (read == 0 || WSAGetLastError() == WSAEWOULDBLOCK) return result;
        return result;
    }
}
} // namespace

namespace sunrise::core::settings {
const Settings& get() noexcept { return g_settings; }
} // namespace sunrise::core::settings

namespace sunrise::core::log {
void write(Channel, Level, std::string_view) noexcept {}
} // namespace sunrise::core::log

int main() {
    registry::shutdown(); queue::shutdown();
    registry::Descriptor command{};
    command.name = "test.ping";
    command.help = "Replies with pong.";
    command.kind = registry::Kind::command;
    command.invoke = &ping;
    assert(registry::register_entry(command) == registry::RegistrationResult::registered);

    g_settings.server.consoleEndpoint.enabled = true;
    g_settings.server.consoleEndpoint.port = free_loopback_port();
    assert(endpoint::initialize());

    WSADATA data{};
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    const SOCKET peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(peer != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(g_settings.server.consoleEndpoint.port);
    assert(connect(peer, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
    u_long nonblocking = 1;
    assert(ioctlsocket(peer, FIONBIO, &nonblocking) == 0);

    constexpr std::string_view first = R"({"id":17,"line":"test.)";
    constexpr std::string_view second = "ping\"}\n";
    assert(send(peer, first.data(), static_cast<int>(first.size()), 0)
           == static_cast<int>(first.size()));
    endpoint::service(1);
    assert(queue::pending() == 0);
    assert(send(peer, second.data(), static_cast<int>(second.size()), 0)
           == static_cast<int>(second.size()));

    std::string response;
    for (std::uint64_t tick = 2; tick < 100 && response.find('\n') == std::string::npos; ++tick) {
        endpoint::service(tick);
        static_cast<void>(queue::drain(nullptr));
        endpoint::service(tick);
        response += receive_available(peer);
    }
    assert(response.find(R"("id":17)") != std::string::npos);
    assert(response.find(R"("status":"ok")") != std::string::npos);
    assert(response.find(R"("summary":"pong")") != std::string::npos);

    // An ordinary peer that sends no further complete request cannot reserve the sole slot forever.
    endpoint::service(40000);
    char byte = 0;
    const int closed = recv(peer, &byte, 1, 0);
    assert(closed == 0 || (closed == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK));

    closesocket(peer);
    WSACleanup();
    endpoint::shutdown();
    queue::shutdown();
    registry::shutdown();
}
