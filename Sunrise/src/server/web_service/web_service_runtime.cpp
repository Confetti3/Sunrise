#include "web_service_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/runtime/server_clock.h"
#include "../../middleware/encoding/bit_reader.h"
#include "../../middleware/encoding/byte_order.h"
#include "../../middleware/web_service/messages/opcode1820.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode205.h"
#include "../../middleware/web_service/messages/opcode206.h"
#include "../../middleware/web_service/messages/opcode402.h"
#include "../../middleware/web_service/messages/opcode403.h"
#include "../../middleware/web_service/messages/opcode406.h"
#include "../../middleware/web_service/messages/opcode501_codec.h"
#include "../../middleware/web_service/messages/opcode503.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode601/opcode601_codec.h"
#include "../../middleware/web_service/messages/opcode701/opcode701_codec.h"
#include "../../middleware/web_service/messages/opcode702.h"
#include "../../middleware/web_service/messages/opcode801.h"
#include "../../middleware/web_service/messages/opcode901/opcode901_codec.h"
#include "../../middleware/web_service/messages/opcode904/opcode904_codec.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../middleware/web_service/web_service_envelope.h"
#include "../../state/account/account_state.h"
#include "../../state/activity/membership/activity_membership_query.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"
#include "internal.h"
#include "opcode_routes.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {
namespace {

namespace messages = middleware::web_service::messages;

/** One ordinary event line carries an opcode and its fixed prefix. */
constexpr std::size_t kOpcodeLineCapacity = 64;
/** A request trace keeps enough payload to identify an item-action descriptor. */
constexpr std::size_t kRequestPayloadTraceBytes = 192;
/** Marks a trace that stopped at the cap, so a short hex string is not read as a short payload. */
constexpr std::string_view kTruncated = " truncated=1";
/** The mutation variant's first alternative is the empty one, so index zero prepared nothing. */
constexpr std::size_t kNoMutation = 0;

/**
 * Logs the Web Service opcode and a bounded payload trace.
 * One svc-10 frame looks like any other, and the opcode drives the client's queuez state machine.
 * @param message Parsed request envelope and borrowed payload.
 */
void report_request(const middleware::web_service::Message& message) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=ws stage=request opcode=%u transaction=%u payload_bytes=%zu payload_hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      message.payload.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }

    std::size_t length = static_cast<std::size_t>(prefix);
    const std::size_t traced =
        (std::min)(message.payload.size(), static_cast<std::size_t>(kRequestPayloadTraceBytes));
    (void)core::log::append_hex(line, length, message.payload.first(traced));
    if (traced != message.payload.size() && length + kTruncated.size() < line.size()) {
        std::memcpy(line.data() + length, kTruncated.data(), kTruncated.size());
        length += kTruncated.size();
    }
    core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), length});
}

/**
 * Answers a request whose own codec refused with the bare correlated echo.
 * The Client matches on the echoed transaction id. A missing body under-runs its decoder and
 * takes the BAP connection down, so a thin body is always sent.
 * @param message Parsed request whose correlation fields are echoed.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size in bytes.
 * @return True when the echo fits.
 */
bool encode_echo(const middleware::web_service::Message& message,
                 std::span<std::byte> response,
                 std::size_t& written) noexcept {
    std::array<char, kOpcodeLineCapacity> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=ws stage=body result=echo opcode=%u", message.opcode);
    report_line(core::log::Level::warn, line, count);
    namespace ws = middleware::web_service;
    return ws::encode_response(
        message, ws::ResponseShape::generic, ws::StatusResponse{}, response, written);
}

/** Narrow semantic result from the prefix of reflected WS-701 schema 0x80807603. */
struct ProfileSetupMarker {
    bool present{};
    bool completed{};
};

/** Reads the presence bit that precedes every optional WS-701 schema node. */
[[nodiscard]] bool read_ws701_presence(middleware::encoding::bits::Reader& reader,
                                       bool& present) noexcept {
    std::uint64_t value = 0;
    if (!reader.read(1, value)) {
        return false;
    }
    present = value != 0;
    return true;
}

/** Consumes one optional fixed-width field without retaining it. */
[[nodiscard]] bool skip_ws701_optional(middleware::encoding::bits::Reader& reader,
                                       std::size_t widthBits) noexcept {
    bool present = false;
    return read_ws701_presence(reader, present) && (!present || reader.skip(widthBits));
}

/**
 * Reads only enough of WS-701 schema 0x80807603 to reach preference path 0.1.1.0.
 *
 * PR #71 maps that first preference scalar as the one-bit profile-setup marker. Everything after
 * it belongs to the broader settings-write implementation and is deliberately left to that work.
 * This function therefore validates the complete prefix, not the remainder of the request.
 */
[[nodiscard]] bool parse_profile_setup_marker(const middleware::web_service::Message& message,
                                              ProfileSetupMarker& output) noexcept {
    output = {};
    if (message.opcode != messages::opcode701::kOpcode) {
        return false;
    }

    middleware::encoding::bits::Reader reader(message.payload);
    bool present = false;

    // 0.0? client metadata.
    if (!read_ws701_presence(reader, present)) {
        return false;
    }
    if (present) {
        // 0.0.0? [128] optional 64-bit publicity expiries.
        bool publicityPresent = false;
        if (!read_ws701_presence(reader, publicityPresent)) {
            return false;
        }
        if (publicityPresent) {
            for (std::size_t index = 0; index < 128; ++index) {
                if (!skip_ws701_optional(reader, 64)) {
                    return false;
                }
            }
        }

        // 0.0.1? [13] required 32-bit seen-message values.
        bool seenMessagesPresent = false;
        if (!read_ws701_presence(reader, seenMessagesPresent)
            || (seenMessagesPresent && !reader.skip(13U * 32U))) {
            return false;
        }
    }

    // 0.1? account data.
    bool accountPresent = false;
    if (!read_ws701_presence(reader, accountPresent)) {
        return false;
    }
    if (!accountPresent) {
        return true;
    }

    // 0.1.0? [2] optional calibration vectors, each containing two required real32 values.
    bool calibrationPresent = false;
    if (!read_ws701_presence(reader, calibrationPresent)) {
        return false;
    }
    if (calibrationPresent) {
        for (std::size_t index = 0; index < 2; ++index) {
            bool vectorPresent = false;
            if (!read_ws701_presence(reader, vectorPresent)
                || (vectorPresent && !reader.skip(2U * 32U))) {
                return false;
            }
        }
    }

    // 0.1.1? preference record.
    bool preferencesPresent = false;
    if (!read_ws701_presence(reader, preferencesPresent)) {
        return false;
    }
    if (!preferencesPresent) {
        return true;
    }

    // 0.1.1.0? one-bit profile-setup marker.
    if (!read_ws701_presence(reader, output.present)) {
        return false;
    }
    if (!output.present) {
        return true;
    }

    std::uint64_t completed = 0;
    if (!reader.read(1, completed)) {
        return false;
    }
    output.completed = completed != 0;
    return true;
}

/**
 * Issues the family-5 server clock the Client extrapolates its family-5 time from.
 * The wire field counts whole seconds. A repeated value reads as no change and stalls the
 * Client's family-5 boot task, so the issued count must strictly increase.
 * @return Unix seconds, always greater than the previous call's result.
 */
[[nodiscard]] std::uint64_t next_family5_clock() noexcept {
    static std::atomic<std::uint64_t> issued{0};
    const auto wall = static_cast<std::uint64_t>(core::runtime::server_clock_seconds());
    std::uint64_t previous = issued.load(std::memory_order_relaxed);
    std::uint64_t next = 0;
    do {
        next = wall > previous ? wall : previous + 1;
    } while (!issued.compare_exchange_weak(previous, next, std::memory_order_relaxed));
    return next;
}

/**
 * Records the world state the character write-back reports.
 * The body is client-owned state; the world-state field is the one value the host acts on.
 * @param message Parsed ws-702 envelope.
 */
void note_character_writeback(const middleware::web_service::Message& message) noexcept {
    messages::opcode702::Request request;
    const bool parsed = messages::opcode702::parse_request(message, request);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=writeback result=%s world_state=%u",
                                      parsed ? "ok" : "unparsed",
                                      static_cast<unsigned>(request.worldState));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    if (parsed) {
        state::activity::membership::note_client_writeback(request.worldState
                                                           == messages::opcode702::kInWorld);
    }
}

/**
 * Applies the report that carries no answer of its own beyond the shared status pair.
 * @param message Parsed request envelope and borrowed payload.
 */
void note_reports(const middleware::web_service::Message& message) noexcept {
    if (message.opcode == messages::opcode702::kOpcode) {
        note_character_writeback(message);
    }
}

} // namespace

/** Re-encodes a prepared reply as a refusal after its Queuez staging failed. */
bool encode_staging_refusal(const middleware::web_service::Message& message,
                            std::span<std::byte> response,
                            std::size_t& written) noexcept {
    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    middleware::web_service::StatusResponse status{};
    status.code = middleware::web_service::kRefusedStatusCode;
    status.value = middleware::web_service::kNoFamily4Publication;
    return middleware::web_service::encode_response(message, shape, status, response, written);
}

/** Answers one Web Service request when its caller has no action to publish. */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    Outcome outcome;
    return consume(request, response, written, outcome);
}

/** Parses one request, prepares any action it names, and encodes the reply that reports it. */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written,
             Outcome& outcome) noexcept {
    written = 0;
    outcome = {};
    middleware::web_service::Message message;
    if (!middleware::web_service::parse_request(request, message)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws stage=parse result=fail");
        return false;
    }
    report_request(message);
    note_reports(message);

    if (message.opcode == messages::opcode205::kOpcode) {
        state::InvestmentState investment{};
        return (state::investment_snapshot(investment)
                && messages::opcode205::encode_response(
                    message, investment, next_family5_clock(), response, written))
               || encode_echo(message, response, written);
    }

    if (message.opcode == messages::opcode503::kOpcode) {
        messages::opcode503::Request bootstrap;
        const bool parsed = messages::opcode503::parse_request(message, bootstrap);
        // The request's own key is echoed and adopted. An authored id here costs the ship and the
        // banner.
        if (!bootstrap.hasPrimarySoid) {
            bootstrap.primarySoid = state::account_snapshot().primarySoid;
        }
        state::InvestmentState investment{};
        if (!parsed || !state::investment_snapshot(investment)
            || !messages::opcode503::encode_response(
                message, bootstrap, investment, next_family5_clock(), response, written)) {
            return encode_echo(message, response, written);
        }
        if (bootstrap.hasPrimarySoid && !state::set_primary_soid(bootstrap.primarySoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=ws503 stage=adopt result=fail");
        }
        return true;
    }

    if (message.opcode == messages::opcode501::kOpcode) {
        // Returns a SOID family three already publishes. The request body is not parsed.
        const std::uint64_t characterSoid =
            state::account::selected_character_soid(state::account_snapshot());
        return messages::opcode501::encode_response(message, characterSoid, response, written)
               || encode_echo(message, response, written);
    }

    // Vendor purchases fall through to the shared response-shape path, which runs the action and
    // answers its status: an action that prepared no mutation is answered with the refused code.

    if (message.opcode == messages::opcode601::kOpcode) {
        return messages::opcode601::encode_response(message, response, written)
               || encode_echo(message, response, written);
    }

    // A subscribe whose body does not parse is still answered; only the subscription is dropped.
    middleware::queuez::Subscription subscription;
    const bool subscribes = message.opcode == messages::opcode206::kOpcode
                            && messages::opcode206::parse_request(message, subscription);

    // The action runs before its reply is encoded, because the reply reports whether it worked.
    // Most actions fill the outcome only after preparing a whole transition. WS-701 also accepts
    // a valid no-op heartbeat, so that one success is tracked separately from mutation presence.
    bool dispatched = true;
    bool acceptedWithoutMutation = false;
    bool profileSetupRefused = false;
    if (message.opcode == messages::opcode504::kOpcode) {
        select_character(message, outcome);
    } else if (message.opcode == messages::opcode402::kOpcode) {
        dismantle_item(message, outcome);
    } else if (message.opcode == messages::opcode403::kOpcode) {
        mutate_equipment(message, false, outcome);
    } else if (message.opcode == messages::opcode403::kUnequipOpcode) {
        mutate_equipment(message, true, outcome);
    } else if (message.opcode == messages::opcode801::kOpcode) {
        mutate_subclass_selection(message, outcome);
    } else if (message.opcode == messages::opcode903::kOpcode) {
        mutate_socket_plug(message, outcome);
    } else if (message.opcode == messages::opcode1901::kOpcode) {
        mutate_equipped_socket_plug(message, outcome);
    } else if (message.opcode == messages::opcode406::kOpcode) {
        mutate_item_state(message, outcome);
    } else if (message.opcode == messages::opcode701::kOpcode) {
        const state::SettingsUpdateDisposition disposition = mutate_settings(message, outcome);
        acceptedWithoutMutation = disposition == state::SettingsUpdateDisposition::acceptedNoChange;
        // The completion marker is applied here. The shared status path below reports the result.
        ProfileSetupMarker marker{};
        const bool parsed = parse_profile_setup_marker(message, marker);
        if (!parsed) {
            // Preserve Sunrise's existing WS-701 success behavior outside this narrow feature.
            // PR #71 owns complete settings-write validation and can later subsume this prefix.
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=ws701 stage=profile_setup result=ignored reason=prefix_parse");
        } else if (marker.present && marker.completed) {
            if (!state::complete_profile_setup()) {
                profileSetupRefused = true;
            } else {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 "ev=ws701 stage=profile_setup result=complete marker=1");
            }
        }
    } else if (message.opcode == messages::opcode1820::kOpcode) {
        acquire_item(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode901::kOpcode) {
        purchase_item(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode904::kOpcode) {
        acquire_quest(message, outcome);
    } else {
        dispatched = false;
    }
    const bool prepared = outcome.hasSelectedCharacter || outcome.mutation.index() != kNoMutation;

    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    middleware::web_service::StatusResponse status{};
    if (awaits_family4_version(message.opcode)) {
        // Nothing is published from here. A staged mutation re-encodes this with its own revision.
        status.value = middleware::web_service::kNoFamily4Publication;
    }
    if ((dispatched && !prepared && !acceptedWithoutMutation) || profileSetupRefused) {
        status.code = middleware::web_service::kRefusedStatusCode;
    }
    if (!middleware::web_service::encode_response(message, shape, status, response, written)) {
        // The echo carries no status, so nothing may be published against it.
        outcome = {};
        return encode_echo(message, response, written);
    }
    if (subscribes) {
        // Publish the subscription only after its correlated response is complete.
        outcome.hasSubscription = true;
        outcome.subscription = subscription;
    }
    return true;
}

} // namespace sunrise::server::web_service
