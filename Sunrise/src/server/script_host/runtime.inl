#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "../../client/hooks/network/bubble_authority/bubble_authority_replacements.h"
#include "../../core/logging/log.h"
#include "../../state/activity/forced/activity_forced_destination.h"
#include "../../state/activity/incidents/runtime.h"
#include "../../state/activity/runtime.h"
#include "../../state/activity/switch_commands/runtime.h"
#include "protocol.h"
#include "runtime.h"

namespace sunrise::server::script_host {
namespace {

#include "detail/runtime_state.inl"
#include "detail/runtime_messages.inl"
#include "detail/runtime_json.inl"
#include "detail/runtime_io.inl"

} // namespace

bool initialize() noexcept {
    if (g_initialized) {
        return true;
    }
    disconnect();
    g_nextConnectTick = 0;
    g_disabled = disabled_by_environment();
    configure_pipe_path();
    g_initialized = true;
    report("initialize", g_disabled ? "disabled" : "ok");
    return true;
}

void service(std::uint64_t now) noexcept {
    if (!g_initialized || g_disabled) {
        return;
    }
    connect_if_due(now);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        return;
    }
    stage_world_phase();
    stage_placed_content_authority();
    stage_activity_incident();
    stage_gameplay_switch();
    if (!flush_output() || !receive_input() || !flush_output()) {
        report("connection", "lost");
        disconnect();
        g_nextConnectTick = now + kReconnectDelayMilliseconds;
    }
}

void shutdown() noexcept {
    if (!g_initialized) {
        return;
    }
    disconnect();
    g_initialized = false;
    g_disabled = false;
    g_nextConnectTick = 0;
    report("shutdown", "ok");
}

} // namespace sunrise::server::script_host
