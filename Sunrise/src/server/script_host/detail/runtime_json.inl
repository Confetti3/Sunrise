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
            enqueue_command_result(request,
                                   "busy",
                                   "Placed-content authority observation is updating.");
            return;
        }
        enqueue_placed_content_authority_result(request, observation);
        return;
    }
    enqueue_command_result(request,
                           "unsupported",
                           "The native bridge does not advertise this capability.");
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

