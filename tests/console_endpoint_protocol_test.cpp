#include "core/console/registry/console_registry.h"
#include "server/console_endpoint/protocol/console_protocol.h"
#include "server/console_endpoint/replies/console_reply_table.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace console = sunrise::core::console;
namespace registry = console::registry;
namespace protocol = sunrise::server::console_endpoint::protocol;
namespace replies = sunrise::server::console_endpoint::replies;

namespace {
[[nodiscard]] bool read_flag(console::Value& output) noexcept {
    output.type = console::Type::boolean;
    output.boolean = true;
    return true;
}
} // namespace

int main() {
    protocol::Request request{};
    assert(protocol::decode_request(R"({"id":1,"line":"console.help test"})", request));
    assert(request.id == 1 && !request.describe
           && std::string_view(request.line.data(), request.lineLength) == "console.help test");
    assert(protocol::decode_request(" { \"id\" : 2 , \"describe\" : true } \r", request));
    assert(request.id == 2 && request.describe);
    assert(!protocol::decode_request(R"({"line":"x","id":3})", request) && request.id == 0);
    assert(!protocol::decode_request(R"({"id":4,"line":"x","describe":true})", request)
           && request.id == 4);
    assert(!protocol::decode_request(R"({"id":5,"id":6,"line":"x"})", request)
           && request.id == 5);
    assert(!protocol::decode_request(R"({"id":0,"line":"x"})", request));
    assert(protocol::decode_request(R"({"id":7,"line":"safety.text \"a\\b\""})", request));
    assert(std::string_view(request.line.data(), request.lineLength) == "safety.text \"a\\b\"");

    console::Result result{};
    result.status = console::Status::ok;
    console::set_summary(result, "ready");
    console::Value value{};
    value.type = console::Type::text;
    console::store_text("a\"b\\c", value.text, value.textLength);
    assert(console::add_row(result, "text", value));
    std::array<char, 4096> encoded{};
    std::size_t length = 0;
    protocol::encode_result(9, result, encoded, length);
    const std::string_view response{encoded.data(), length};
    assert(length != 0 && response.starts_with(R"({"id":9,"status":"ok")")
           && response.find(R"("summary":"ready")") != std::string_view::npos
           && response.find(R"("value":"a\"b\\c")") != std::string_view::npos
           && response.ends_with("}"));

    // Public result fields are callback-filled, so malformed counts must still remain bounded.
    result.summaryLength = (std::numeric_limits<std::size_t>::max)();
    result.rowCount = (std::numeric_limits<std::size_t>::max)();
    result.rows[0].keyLength = (std::numeric_limits<std::size_t>::max)();
    result.rows[0].value.type = console::Type::text;
    result.rows[0].value.textLength = (std::numeric_limits<std::size_t>::max)();
    protocol::encode_result(10, result, encoded, length);
    assert(length > 0 && length <= encoded.size() && encoded[length - 1] == '}');
    std::array<char, protocol::kMinimumCapacity - 1> tooSmall{};
    length = 99;
    protocol::encode_result(1, result, tooSmall, length);
    assert(length == 0);

    registry::shutdown();
    registry::Descriptor flag{};
    flag.name = "test.flag";
    flag.help = "A test flag.";
    flag.kind = registry::Kind::variable;
    flag.type = console::Type::boolean;
    flag.read = &read_flag;
    assert(registry::register_entry(flag) == registry::RegistrationResult::registered);
    std::array<char, 4096> registryJson{};
    protocol::encode_registry(11, registryJson, length);
    const std::string_view described{registryJson.data(), length};
    assert(described.find(R"("name":"test.flag")") != std::string_view::npos
           && described.find(R"("kind":"variable")") != std::string_view::npos);
    registry::shutdown();

    replies::clear();
    console::Result first{}; first.status = console::Status::ok;
    console::Result second{}; second.status = console::Status::failed;
    assert(!replies::remember(0, first));
    assert(replies::remember(1, first) && replies::remember(2, second));
    console::Result taken{};
    assert(replies::take(1, taken) && taken.status == console::Status::ok);
    assert(!replies::take(1, taken));
    assert(replies::take(2, taken) && taken.status == console::Status::failed);
    replies::clear();
}
