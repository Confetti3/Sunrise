void skip_whitespace(std::string_view source, std::size_t& cursor) noexcept {
    while (cursor < source.size()) {
        const unsigned char character = static_cast<unsigned char>(source[cursor]);
        if (character != ' ' && character != '\t' && character != '\r'
            && character != '\n') {
            break;
        }
        ++cursor;
    }
}

[[nodiscard]] bool parse_string(std::string_view source,
                                std::size_t& cursor,
                                std::span<char> output,
                                std::size_t& outputSize) noexcept {
    outputSize = 0;
    if (cursor >= source.size() || source[cursor] != '"') {
        return false;
    }
    ++cursor;
    while (cursor < source.size()) {
        char character = source[cursor++];
        if (character == '"') {
            return true;
        }
        if (character == '\\') {
            if (cursor >= source.size()) {
                return false;
            }
            const char escape = source[cursor++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                character = escape;
                break;
            case 'b':
                character = '\b';
                break;
            case 'f':
                character = '\f';
                break;
            case 'n':
                character = '\n';
                break;
            case 'r':
                character = '\r';
                break;
            case 't':
                character = '\t';
                break;
            default:
                return false;
            }
        }
        if (outputSize == output.size()) {
            return false;
        }
        output[outputSize++] = character;
    }
    return false;
}

[[nodiscard]] bool locate_member(std::string_view source,
                                 std::string_view key,
                                 std::size_t& valueCursor) noexcept {
    for (std::size_t scan = 0; scan < source.size();) {
        if (source[scan] != '"') {
            ++scan;
            continue;
        }
        std::array<char, 64> parsedKey{};
        std::size_t parsedKeySize = 0;
        std::size_t cursor = scan;
        if (!parse_string(source, cursor, parsedKey, parsedKeySize)) {
            ++scan;
            continue;
        }
        skip_whitespace(source, cursor);
        if (cursor < source.size() && source[cursor] == ':') {
            ++cursor;
            skip_whitespace(source, cursor);
            if (std::string_view(parsedKey.data(), parsedKeySize) == key) {
                valueCursor = cursor;
                return true;
            }
        }
        scan = cursor > scan ? cursor : scan + 1;
    }
    return false;
}

template <std::size_t Size>
[[nodiscard]] bool find_string_member(std::string_view source,
                                      std::string_view key,
                                      std::array<char, Size>& value,
                                      std::size_t& valueSize) noexcept {
    std::size_t cursor = 0;
    return locate_member(source, key, cursor)
           && parse_string(source, cursor, value, valueSize);
}

[[nodiscard]] bool find_protocol_version(std::string_view source,
                                         std::uint32_t& version) noexcept {
    std::size_t cursor = 0;
    if (!locate_member(source, "protocol", cursor)) {
        return false;
    }
    std::uint32_t result = 0;
    std::size_t digitCount = 0;
    while (cursor < source.size() && source[cursor] >= '0' && source[cursor] <= '9') {
        result = result * 10U + static_cast<std::uint32_t>(source[cursor] - '0');
        ++cursor;
        ++digitCount;
    }
    if (digitCount == 0) {
        return false;
    }
    version = result;
    return true;
}

[[nodiscard]] bool parse_unsigned(std::string_view text,
                                  std::uint64_t maximum,
                                  std::uint64_t& output) noexcept {
    output = 0;
    unsigned base = 10;
    std::size_t index = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        index = 2;
    }
    if (index == text.size()) {
        return false;
    }
    for (; index < text.size(); ++index) {
        const char character = text[index];
        unsigned digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<unsigned>(character - '0');
        } else if (base == 16 && character >= 'a' && character <= 'f') {
            digit = static_cast<unsigned>(character - 'a') + 10U;
        } else if (base == 16 && character >= 'A' && character <= 'F') {
            digit = static_cast<unsigned>(character - 'A') + 10U;
        } else {
            return false;
        }
        if (digit >= base || output > (maximum - digit) / base) {
            return false;
        }
        output = output * base + digit;
    }
    return true;
}

[[nodiscard]] bool extract_json_value(std::string_view line,
                                      std::string_view key,
                                      std::string_view& output) noexcept {
    output = {};
    std::array<char, 64> token{};
    const int tokenLength = std::snprintf(
        token.data(), token.size(), "\"%.*s\"", static_cast<int>(key.size()), key.data());
    if (tokenLength <= 0 || static_cast<std::size_t>(tokenLength) >= token.size()) {
        return false;
    }
    const std::size_t keyPosition =
        line.find(std::string_view(token.data(), static_cast<std::size_t>(tokenLength)));
    const std::size_t colon = keyPosition == std::string_view::npos
                                  ? std::string_view::npos
                                  : line.find(':', keyPosition + tokenLength);
    if (colon == std::string_view::npos) {
        return false;
    }
    std::size_t first = colon + 1;
    while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) {
        ++first;
    }
    if (first == line.size()) {
        return false;
    }
    if (line[first] == '"') {
        const std::size_t last = line.find('"', first + 1);
        if (last == std::string_view::npos || last == first + 1) {
            return false;
        }
        output = line.substr(first + 1, last - first - 1);
        return true;
    }
    std::size_t last = first;
    while (last < line.size() && line[last] != ',' && line[last] != '}'
           && line[last] != ' ' && line[last] != '\t' && line[last] != '\r'
           && line[last] != '\n') {
        ++last;
    }
    if (last == first) {
        return false;
    }
    output = line.substr(first, last - first);
    return true;
}

[[nodiscard]] bool extract_json_unsigned(std::string_view line,
                                         std::string_view key,
                                         std::uint64_t maximum,
                                         std::uint64_t& output) noexcept {
    std::string_view value;
    return extract_json_value(line, key, value) && parse_unsigned(value, maximum, output);
}

[[nodiscard]] bool extract_json_boolean(std::string_view line,
                                        std::string_view key,
                                        bool& output) noexcept {
    std::string_view value;
    if (!extract_json_value(line, key, value)) {
        return false;
    }
    if (value == "true") {
        output = true;
        return true;
    }
    if (value == "false") {
        output = false;
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_package_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > state::activity::destination::kPackageNameCapacity) {
        return false;
    }
    for (const char character : name) {
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')
              || character == '_')) {
            return false;
        }
    }
    return true;
}

void handle_activity_override(std::string_view request, std::string_view line) noexcept {
    bool enabled = false;
    if (!extract_json_boolean(line, "enabled", enabled)) {
        enqueue_command_result(request, "error", "enabled must be a boolean");
        return;
    }
    if (!enabled) {
        state::activity::forced::clear();
        report("activity_override", "cleared");
        enqueue_command_result(request, "ok", "override cleared");
        return;
    }

    std::array<char, 96> package{};
    std::size_t packageLength = 0;
    std::uint64_t bubble = 0;
    std::uint64_t sliceSet = 0;
    if (!find_string_member(line, "packageName", package, packageLength)
        || !valid_package_name(std::string_view(package.data(), packageLength))
        || !extract_json_unsigned(
            line, "bubble", state::activity::forced::kMaximumBubble, bubble)
        || !extract_json_unsigned(
            line, "sliceSet", state::activity::forced::kMaximumSliceSet, sliceSet)) {
        enqueue_command_result(request, "error", "packageName, bubble, or sliceSet is invalid");
        return;
    }

    state::activity::forced::ForcedDestination value{};
    std::copy_n(package.begin(), packageLength, value.packageName.begin());
    value.packageNameLength = static_cast<std::uint8_t>(packageLength);
    value.bubble = static_cast<std::uint8_t>(bubble);
    value.hasBubble = true;
    value.sliceSet = static_cast<std::uint16_t>(sliceSet);
    value.hasSliceSet = true;
    value.enabled = true;
    if (line.find(R"("spawnSetHash")") != std::string_view::npos) {
        std::uint64_t spawnSet = 0;
        if (!extract_json_unsigned(
                line,
                "spawnSetHash",
                (std::numeric_limits<std::uint32_t>::max)(),
                spawnSet)) {
            enqueue_command_result(request, "error", "spawnSetHash is invalid");
            return;
        }
        value.spawnSetHash = static_cast<std::uint32_t>(spawnSet);
        value.hasSpawnSetHash = true;
    }
    if (!state::activity::forced::publish(value)) {
        enqueue_command_result(request, "error", "override failed state validation");
        return;
    }
    report("activity_override", "published");
    enqueue_command_result(request, "ok", "override published");
}

void handle_gameplay_switch(std::string_view request, std::string_view line) noexcept {
    constexpr std::uint64_t kResultTimeoutMilliseconds = 2'000;
    constexpr std::uint64_t kInlinePollMilliseconds = 150;
    std::uint64_t definition = 0;
    std::uint64_t value = 0;
    if (!extract_json_unsigned(
            line,
            "definitionIndex",
            (std::numeric_limits<std::uint16_t>::max)(),
            definition)
        || !extract_json_unsigned(line, "value", 2, value)) {
        enqueue_command_result(request, "error", "definitionIndex or value is invalid");
        return;
    }
    if (definition != 0x00F6) {
        enqueue_command_result(
            request, "error", "only verified Homecoming definition 0xF6 is accepted");
        return;
    }
    if (state::activity::world_phase() != state::activity::WorldPhase::arrived) {
        enqueue_command_result(request, "error", "client is not in an arrived world");
        return;
    }

    std::uint64_t sequence = 0;
    if (!state::activity::switch_commands::publish(
            static_cast<std::uint16_t>(definition),
            static_cast<std::int32_t>(value),
            sequence)) {
        enqueue_command_result(request, "error", "native command slot is busy");
        return;
    }

    const std::uint64_t started = GetTickCount64();
    state::activity::switch_commands::Result result{};
    while (GetTickCount64() - started < kInlinePollMilliseconds) {
        if (state::activity::switch_commands::try_take_result(sequence, result)) {
            std::array<char, 128> reason{};
            const int written = std::snprintf(reason.data(),
                                              reason.size(),
                                              "definition 0x%X changed %u to %u",
                                              static_cast<unsigned int>(definition),
                                              result.before,
                                              result.after);
            if (!result.applied || written <= 0
                || static_cast<std::size_t>(written) >= reason.size()) {
                enqueue_command_result(
                    request, "error", "native writer rejected the retained switch");
                return;
            }
            enqueue_command_result(
                request,
                "ok",
                std::string_view(reason.data(), static_cast<std::size_t>(written)));
            return;
        }
        Sleep(1);
    }

    // Defer completion to a follow-up service tick; never stall the draw thread.
    g_pendingSwitchRequest.fill('\0');
    std::memcpy(g_pendingSwitchRequest.data(), request.data(), request.size());
    g_pendingSwitchRequestSize = request.size();
    g_pendingSwitchSequence = sequence;
    g_pendingSwitchDeadline = GetTickCount64() + kResultTimeoutMilliseconds;
    g_hasPendingSwitch = true;
}

void process_command(std::string_view line) noexcept {
    std::array<char, 96> requestId{};
    std::array<char, 96> capability{};
    std::size_t requestIdSize = 0;
    std::size_t capabilitySize = 0;
    if (!find_string_member(line, "requestId", requestId, requestIdSize)
        || !find_string_member(line, "capability", capability, capabilitySize)) {
        report("command", "invalid");
        return;
    }

    const std::string_view request(requestId.data(), requestIdSize);
    const std::string_view requestedCapability(capability.data(), capabilitySize);
    if (!valid_token(request) || !valid_token(requestedCapability)) {
        report("command", "invalid-token");
        return;
    }

    if (requestedCapability == protocol::kCapabilityPing) {
        enqueue_command_result(request, "ok", {}, R"({"pong":true})");
        return;
    }
    if (requestedCapability == protocol::kCapabilityWorldPhaseObserve) {
        enqueue_world_phase_result(request);
        return;
    }
    if (requestedCapability == protocol::kCapabilityPlacedContentAuthorityObserve) {
        client::hooks::network::bubble_authority::AuthorityObservation observation{};
        if (!client::hooks::network::bubble_authority::try_observation(observation)) {
            enqueue_command_result(
                request, "busy", "Placed-content authority observation is updating.");
            return;
        }
        enqueue_placed_content_authority_result(request, observation);
        return;
    }
    if (requestedCapability == protocol::kCapabilityActivityOverrideConfigure) {
        handle_activity_override(request, line);
        return;
    }
    if (requestedCapability == protocol::kCapabilityGameplaySwitchSet) {
        handle_gameplay_switch(request, line);
        return;
    }
    enqueue_command_result(
        request, "unsupported", "The native bridge does not advertise this capability.");
}

void process_line(std::string_view line) noexcept {
    std::uint32_t version = 0;
    std::array<char, 64> type{};
    std::size_t typeSize = 0;
    if (!find_protocol_version(line, version) || version != protocol::kVersion
        || !find_string_member(line, "type", type, typeSize)) {
        report("protocol", "invalid");
        return;
    }

    const std::string_view messageType(type.data(), typeSize);
    if (messageType == "host.capabilities") {
        enqueue_hello();
    } else if (messageType == "host.ping") {
        enqueue_pong();
    } else if (messageType == "command.request") {
        process_command(line);
    } else {
        report("protocol", "unknown-type");
    }
}

