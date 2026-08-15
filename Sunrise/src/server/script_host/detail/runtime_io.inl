[[nodiscard]] bool parse_lines() noexcept {
    std::size_t parsed = 0;
    while (parsed < protocol::kMaximumLinesPerService) {
        const auto begin = g_receive.begin();
        const auto newline = std::find(begin, begin + g_receiveSize, '\n');
        if (newline == begin + g_receiveSize) {
            return g_receiveSize <= protocol::kMaximumLineSize;
        }

        std::size_t lineSize = static_cast<std::size_t>(newline - begin);
        if (lineSize > protocol::kMaximumLineSize) {
            report("receive", "line-too-long");
            return false;
        }
        if (lineSize != 0 && g_receive[lineSize - 1] == '\r') {
            --lineSize;
        }
        if (lineSize != 0) {
            process_line(std::string_view(g_receive.data(), lineSize));
        }

        const std::size_t consumed = static_cast<std::size_t>(newline - begin) + 1;
        const std::size_t remaining = g_receiveSize - consumed;
        if (remaining != 0) {
            std::memmove(g_receive.data(), g_receive.data() + consumed, remaining);
        }
        std::fill(g_receive.begin() + static_cast<std::ptrdiff_t>(remaining),
                  g_receive.begin() + static_cast<std::ptrdiff_t>(g_receiveSize),
                  '\0');
        g_receiveSize = remaining;
        ++parsed;
    }
    return true;
}

[[nodiscard]] bool receive_input() noexcept {
    for (std::size_t operation = 0;
         operation < protocol::kMaximumIoOperationsPerService;
         ++operation) {
        DWORD available = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }
        if (available == 0) {
            return true;
        }
        if (g_receiveSize == g_receive.size()) {
            report("receive", "overflow");
            return false;
        }

        const DWORD capacity = static_cast<DWORD>(g_receive.size() - g_receiveSize);
        const DWORD requested = (std::min)(available, capacity);
        DWORD read = 0;
        if (!ReadFile(g_pipe, g_receive.data() + g_receiveSize, requested, &read, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_DATA || error == ERROR_PIPE_LISTENING) {
                return true;
            }
            return false;
        }
        if (read == 0) {
            return true;
        }
        g_receiveSize += read;
        if (!parse_lines()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool flush_output() noexcept {
    for (std::size_t operation = 0;
         operation < protocol::kMaximumIoOperationsPerService && g_outboundCount != 0;
         ++operation) {
        OutboundLine& line = g_outbound[g_outboundHead];
        const DWORD remaining = line.size - line.offset;
        DWORD written = 0;
        if (!WriteFile(g_pipe, line.bytes.data() + line.offset, remaining, &written, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY
                || error == ERROR_PIPE_LISTENING) {
                return true;
            }
            return false;
        }
        if (written == 0) {
            return true;
        }
        line.offset += written;
        if (line.offset == line.size) {
            line = {};
            g_outboundHead = (g_outboundHead + 1) % g_outbound.size();
            --g_outboundCount;
        }
    }
    return true;
}

void connect_if_due(std::uint64_t now) noexcept {
    if (g_disabled || g_pipe != INVALID_HANDLE_VALUE || now < g_nextConnectTick) {
        return;
    }

    HANDLE pipe = CreateFileW(g_pipePath.data(),
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        g_nextConnectTick = now + kReconnectDelayMilliseconds;
        return;
    }

    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        CloseHandle(pipe);
        g_nextConnectTick = now + kReconnectDelayMilliseconds;
        return;
    }

    g_pipe = pipe;
    clear_transport_buffers();
    if (state::activity::incidents::discard_pending() != 0) {
        report("incident_relay", "stale-discarded");
    }
    enqueue_hello();
    (void)enqueue_world_phase();
    report("connect", "ok");
}

void stage_world_phase() noexcept {
    const state::activity::WorldPhase phase = state::activity::world_phase();
    if (!g_hasWorldPhase || phase != g_lastWorldPhase) {
        (void)enqueue_world_phase();
    }
}

void stage_placed_content_authority() noexcept {
    client::hooks::network::bubble_authority::AuthorityObservation observation{};
    if (!client::hooks::network::bubble_authority::try_observation(observation)) {
        return;
    }
    if (!g_hasPlacedContentAuthority
        || observation.decodeCount != g_lastPlacedContentAuthority.decodeCount
        || observation.droppedCount != g_lastPlacedContentAuthority.droppedCount) {
        (void)enqueue_placed_content_authority(observation);
    }
}

void stage_activity_incident() noexcept {
    for (std::size_t operation = 0;
         operation < protocol::kMaximumLinesPerService;
         ++operation) {
        state::activity::incidents::Observation incident{};
        if (!state::activity::incidents::try_pop(incident)) {
            return;
        }
        if (!enqueue_activity_incident(incident)) {
            report("incident_relay", "drop");
            return;
        }
    }
}

void stage_gameplay_switch() noexcept {
    if (!g_hasPendingSwitch) {
        return;
    }
    state::activity::switch_commands::Result result{};
    if (state::activity::switch_commands::try_take_result(g_pendingSwitchSequence, result)) {
        const std::string_view request(g_pendingSwitchRequest.data(),
                                       g_pendingSwitchRequestSize);
        enqueue_gameplay_switch_result(request, result);
        g_hasPendingSwitch = false;
        return;
    }
    if (GetTickCount64() >= g_pendingSwitchDeadline) {
        const std::string_view request(g_pendingSwitchRequest.data(),
                                       g_pendingSwitchRequestSize);
        if (state::activity::switch_commands::cancel(g_pendingSwitchSequence)) {
            enqueue_command_result(
                request, "error", "timed out before the game thread claimed the command");
        } else {
            enqueue_command_result(
                request, "error", "claimed native command did not finish");
        }
        g_hasPendingSwitch = false;
    }
}

