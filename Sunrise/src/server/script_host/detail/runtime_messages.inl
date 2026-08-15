[[nodiscard]] bool enqueue(std::string_view json) noexcept {
    if (json.size() > protocol::kMaximumLineSize
        || g_outboundCount == g_outbound.size()) {
        report("outbound", "drop");
        return false;
    }

    const std::size_t tail = (g_outboundHead + g_outboundCount) % g_outbound.size();
    OutboundLine& line = g_outbound[tail];
    line = {};
    std::memcpy(line.bytes.data(), json.data(), json.size());
    line.bytes[json.size()] = '\n';
    line.size = static_cast<std::uint32_t>(json.size() + 1);
    ++g_outboundCount;
    return true;
}

void enqueue_hello() noexcept {
    (void)enqueue(R"({"protocol":1,"type":"bridge.hello","build":"Sunrise 0.2.1",)"
                  R"("capabilities":["host.ping","world.phase.observe","activity.snapshot",)"
                  R"("placed-content.authority.observe","activity.incident.observe",)"
                  R"("activity.override.configure","gameplay-switch.read"]})");
}

[[nodiscard]] constexpr std::string_view phase_name(
    state::activity::WorldPhase phase) noexcept {
    switch (phase) {
    case state::activity::WorldPhase::idle:
        return "idle";
    case state::activity::WorldPhase::transitioning:
        return "transitioning";
    case state::activity::WorldPhase::arrived:
        return "arrived";
    }
    return "unknown";
}

[[nodiscard]] bool enqueue_world_phase() noexcept {
    const state::activity::WorldPhase phase = state::activity::world_phase();
    const std::string_view name = phase_name(phase);
    std::array<char, 192> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        R"({"protocol":1,"type":"world.phase","phase":"%.*s","transitionAgeMs":%llu})",
        static_cast<int>(name.size()),
        name.data(),
        static_cast<unsigned long long>(state::activity::world_transition_age()));
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return false;
    }
    if (!enqueue(std::string_view(line.data(), static_cast<std::size_t>(written)))) {
        return false;
    }
    g_lastWorldPhase = phase;
    g_hasWorldPhase = true;
    return true;
}

void enqueue_pong() noexcept {
    (void)enqueue(R"({"protocol":1,"type":"bridge.pong"})");
}

[[nodiscard]] bool valid_token(std::string_view value) noexcept {
    if (value.empty() || value.size() > 96) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_'
               || character == '.';
    });
}

void enqueue_command_result(std::string_view requestId,
                            std::string_view status,
                            std::string_view reason,
                            std::string_view resultJson = {}) noexcept {
    if (!valid_token(requestId)) {
        report("command", "bad-request-id");
        return;
    }

    std::array<char, 768> line{};
    int written = 0;
    if (resultJson.empty()) {
        written = std::snprintf(
            line.data(),
            line.size(),
            R"({"protocol":1,"type":"command.result",)"
            R"("requestId":"%.*s","status":"%.*s","reason":"%.*s"})",
            static_cast<int>(requestId.size()),
            requestId.data(),
            static_cast<int>(status.size()),
            status.data(),
            static_cast<int>(reason.size()),
            reason.data());
    } else {
        written = std::snprintf(
            line.data(),
            line.size(),
            R"({"protocol":1,"type":"command.result",)"
            R"("requestId":"%.*s","status":"%.*s","reason":null,"result":%.*s})",
            static_cast<int>(requestId.size()),
            requestId.data(),
            static_cast<int>(status.size()),
            status.data(),
            static_cast<int>(resultJson.size()),
            resultJson.data());
    }
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        (void)enqueue(std::string_view(line.data(), static_cast<std::size_t>(written)));
    }
}

void enqueue_world_phase_result(std::string_view requestId) noexcept {
    const state::activity::WorldPhase phase = state::activity::world_phase();
    const std::string_view name = phase_name(phase);
    std::array<char, 192> result{};
    const int written = std::snprintf(
        result.data(),
        result.size(),
        R"({"phase":"%.*s","transitionAgeMs":%llu})",
        static_cast<int>(name.size()),
        name.data(),
        static_cast<unsigned long long>(state::activity::world_transition_age()));
    if (written > 0 && static_cast<std::size_t>(written) < result.size()) {
        enqueue_command_result(requestId,
                               "ok",
                               {},
                               std::string_view(result.data(),
                                                static_cast<std::size_t>(written)));
    }
}

void enqueue_activity_snapshot_result(std::string_view requestId) noexcept {
    const state::activity::WorldPhase phase = state::activity::world_phase();
    const std::string_view name = phase_name(phase);
    state::activity::RuntimeSnapshot snapshot{};
    const bool active = state::activity::latest_snapshot(snapshot);
    std::array<char, 768> result{};
    int written = 0;
    if (!active) {
        written = std::snprintf(
            result.data(),
            result.size(),
            R"({"active":false,"worldPhase":"%.*s","transitionAgeMs":%llu,)"
            R"("sessionId":null,"joined":false,"package":null,"activityIndex":null,)"
            R"("arrivalBubbleHash":null,"reportedRegion":null,"heldEntitySlots":0})",
            static_cast<int>(name.size()),
            name.data(),
            static_cast<unsigned long long>(state::activity::world_transition_age()));
    } else {
        std::array<char, 16> activityIndex{};
        const int activityIndexLength = snapshot.destination.activityIndex
                                                == state::activity::destination::
                                                       kAbsentActivityIndex
                                            ? std::snprintf(activityIndex.data(),
                                                            activityIndex.size(),
                                                            "null")
                                            : std::snprintf(
                                                  activityIndex.data(),
                                                  activityIndex.size(),
                                                  "%d",
                                                  snapshot.destination.activityIndex);
        std::array<char, 16> arrivalBubbleHash{};
        const int arrivalBubbleHashLength =
            snapshot.destination.hasArrivalBubbleHash
                ? std::snprintf(arrivalBubbleHash.data(),
                                arrivalBubbleHash.size(),
                                R"("0x%08X")",
                                snapshot.destination.arrivalBubbleHash)
                : std::snprintf(
                      arrivalBubbleHash.data(), arrivalBubbleHash.size(), "null");
        if (activityIndexLength <= 0
            || static_cast<std::size_t>(activityIndexLength) >= activityIndex.size()
            || arrivalBubbleHashLength <= 0
            || static_cast<std::size_t>(arrivalBubbleHashLength)
                   >= arrivalBubbleHash.size()) {
            enqueue_command_result(requestId, "error", "activity snapshot encoding failed");
            return;
        }
        written = std::snprintf(
            result.data(),
            result.size(),
            R"({"active":true,"worldPhase":"%.*s","transitionAgeMs":%llu,)"
            R"("sessionId":"%llu","joined":%s,"package":"%.*s","activityIndex":%s,)"
            R"("arrivalBubbleHash":%s,"reportedRegion":%d,"heldEntitySlots":%u})",
            static_cast<int>(name.size()),
            name.data(),
            static_cast<unsigned long long>(state::activity::world_transition_age()),
            static_cast<unsigned long long>(snapshot.sessionId),
            snapshot.joined ? "true" : "false",
            snapshot.destination.packageNameLength,
            reinterpret_cast<const char*>(snapshot.destination.packageName.data()),
            activityIndex.data(),
            arrivalBubbleHash.data(),
            snapshot.reportedRegion,
            snapshot.heldEntitySlots);
    }
    if (written <= 0 || static_cast<std::size_t>(written) >= result.size()) {
        enqueue_command_result(requestId, "error", "activity snapshot encoding failed");
        return;
    }
    enqueue_command_result(
        requestId,
        "ok",
        {},
        std::string_view(result.data(), static_cast<std::size_t>(written)));
}

void enqueue_gameplay_switch_result(
    std::string_view requestId,
    const state::activity::switch_commands::Result& switchResult) noexcept {
    const bool reading =
        switchResult.operation == state::activity::switch_commands::Operation::read;
    if (!switchResult.applied) {
        enqueue_command_result(
            requestId,
            "error",
            reading ? "native reader rejected the retained switch"
                    : "native writer rejected the retained switch");
        return;
    }

    std::array<char, 192> result{};
    const int written = std::snprintf(
        result.data(),
        result.size(),
        R"({"operation":"%s","definitionIndex":246,"before":%u,"after":%u,"applied":true})",
        reading ? "read" : "set",
        switchResult.before,
        switchResult.after);
    if (written <= 0 || static_cast<std::size_t>(written) >= result.size()) {
        enqueue_command_result(requestId, "error", "gameplay switch result encoding failed");
        return;
    }
    enqueue_command_result(
        requestId,
        "ok",
        {},
        std::string_view(result.data(), static_cast<std::size_t>(written)));
}

[[nodiscard]] bool enqueue_placed_content_authority(
    const client::hooks::network::bubble_authority::AuthorityObservation& observation) noexcept {
    std::array<char, 384> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        R"({"protocol":1,"type":"placed-content.authority",)"
        R"("decodeCount":%llu,"forcedReadCount":%llu,"lastDecoderForcedReads":%llu,)"
        R"("droppedCount":%llu,"lastDecoderSucceeded":%s})",
        static_cast<unsigned long long>(observation.decodeCount),
        static_cast<unsigned long long>(observation.forcedReadCount),
        static_cast<unsigned long long>(observation.lastDecoderForcedReads),
        static_cast<unsigned long long>(observation.droppedCount),
        observation.lastDecoderSucceeded ? "true" : "false");
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return false;
    }
    if (!enqueue(std::string_view(line.data(), static_cast<std::size_t>(written)))) {
        return false;
    }
    g_lastPlacedContentAuthority = observation;
    g_hasPlacedContentAuthority = true;
    return true;
}

void enqueue_placed_content_authority_result(
    std::string_view requestId,
    const client::hooks::network::bubble_authority::AuthorityObservation& observation) noexcept {
    std::array<char, 384> result{};
    const int written = std::snprintf(
        result.data(),
        result.size(),
        R"({"decodeCount":%llu,"forcedReadCount":%llu,"lastDecoderForcedReads":%llu,)"
        R"("droppedCount":%llu,"lastDecoderSucceeded":%s})",
        static_cast<unsigned long long>(observation.decodeCount),
        static_cast<unsigned long long>(observation.forcedReadCount),
        static_cast<unsigned long long>(observation.lastDecoderForcedReads),
        static_cast<unsigned long long>(observation.droppedCount),
        observation.lastDecoderSucceeded ? "true" : "false");
    if (written > 0 && static_cast<std::size_t>(written) < result.size()) {
        enqueue_command_result(requestId,
                               "ok",
                               {},
                               std::string_view(result.data(),
                                                static_cast<std::size_t>(written)));
    }
}

template <std::size_t Capacity, typename... Arguments>
[[nodiscard]] bool append_format(std::array<char, Capacity>& output,
                                 std::size_t& length,
                                 const char* format,
                                 Arguments... arguments) noexcept {
    if (length >= output.size()) {
        return false;
    }
    const std::size_t remaining = output.size() - length;
    const int written = std::snprintf(output.data() + length, remaining, format, arguments...);
    if (written <= 0 || static_cast<std::size_t>(written) >= remaining) {
        return false;
    }
    length += static_cast<std::size_t>(written);
    return true;
}

[[nodiscard]] bool enqueue_activity_incident(
    const state::activity::incidents::Observation& observation) noexcept {
    std::array<char, 1024> line{};
    std::size_t length = 0;
    if (!append_format(
            line,
            length,
            R"({"protocol":1,"type":"activity.incident","sequence":%llu,"observedAtTickMs":%llu,"sessionId":"%llu","accountHandle":"0x%llX","primaryTarget":%u,"extraTargetCount":%u,"extraTargets":[)",
            static_cast<unsigned long long>(observation.sequence),
            static_cast<unsigned long long>(observation.observedAtTickMs),
            static_cast<unsigned long long>(observation.sessionId),
            static_cast<unsigned long long>(observation.accountHandle),
            observation.primaryTarget,
            observation.extraTargetCount)) {
        return false;
    }
    for (std::uint32_t index = 0; index < observation.extraTargetCount; ++index) {
        if (!append_format(
                line, length, index == 0 ? "%u" : ",%u", observation.extraTargets[index])) {
            return false;
        }
    }
    if (!append_format(
            line,
            length,
            R"(],"payloadLength":%u,"bodyLength":%u,"bodyFingerprint":"0x%016llX","hasCompressedSelector":%s,"hasPayload":%s,"droppedBefore":%llu})",
            observation.payloadLength,
            observation.bodyLength,
            static_cast<unsigned long long>(observation.bodyFingerprint),
            observation.hasCompressedSelector ? "true" : "false",
            observation.hasPayload ? "true" : "false",
            static_cast<unsigned long long>(observation.droppedBefore))) {
        return false;
    }
    return enqueue(std::string_view(line.data(), length));
}

