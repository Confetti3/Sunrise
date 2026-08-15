constexpr std::uint64_t kReconnectDelayMilliseconds = 1'000;

struct OutboundLine final {
    std::array<char, protocol::kMaximumLineSize + 1> bytes{};
    std::uint32_t size{};
    std::uint32_t offset{};
};

HANDLE g_pipe{INVALID_HANDLE_VALUE};
std::array<wchar_t, protocol::kMaximumPipePath> g_pipePath{};
std::array<OutboundLine, protocol::kOutboundCapacity> g_outbound{};
std::size_t g_outboundHead{};
std::size_t g_outboundCount{};
std::array<char, protocol::kReceiveCapacity> g_receive{};
std::size_t g_receiveSize{};
std::uint64_t g_nextConnectTick{};
state::activity::WorldPhase g_lastWorldPhase{state::activity::WorldPhase::idle};
bool g_hasWorldPhase{};
bool g_initialized{};
bool g_disabled{};

void report(std::string_view stage, std::string_view result) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=script_host stage=%.*s result=%.*s",
                                      static_cast<int>(stage.size()),
                                      stage.data(),
                                      static_cast<int>(result.size()),
                                      result.data());
    if (written <= 0) {
        return;
    }
    const std::size_t size =
        (std::min)(static_cast<std::size_t>(written), line.size() - 1);
    core::log::write(core::log::Channel::server,
                     core::log::Level::info,
                     std::string_view(line.data(), size));
}

void clear_transport_buffers() noexcept {
    for (auto& line : g_outbound) {
        line = {};
    }
    g_outboundHead = 0;
    g_outboundCount = 0;
    g_receive.fill('\0');
    g_receiveSize = 0;
}

void disconnect() noexcept {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    clear_transport_buffers();
    g_hasWorldPhase = false;
}

[[nodiscard]] bool copy_pipe_path(std::wstring_view value) noexcept {
    if (value.empty() || value.size() >= g_pipePath.size()) {
        return false;
    }
    g_pipePath.fill(L'\0');
    std::copy(value.begin(), value.end(), g_pipePath.begin());
    return true;
}

void configure_pipe_path() noexcept {
    (void)copy_pipe_path(protocol::kDefaultPipeName);

    std::array<wchar_t, protocol::kMaximumPipePath> environment{};
    const DWORD length = GetEnvironmentVariableW(protocol::kPipeEnvironment,
                                                  environment.data(),
                                                  static_cast<DWORD>(environment.size()));
    if (length == 0) {
        return;
    }
    if (length >= environment.size()) {
        report("pipe-config", "too-long");
        return;
    }

    const std::wstring_view value(environment.data(), length);
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\";
    if (value.starts_with(prefix)) {
        if (!copy_pipe_path(value)) {
            report("pipe-config", "invalid");
        }
        return;
    }
    if (value.find(L'\\') != std::wstring_view::npos
        || value.find(L'/') != std::wstring_view::npos
        || prefix.size() + value.size() >= g_pipePath.size()) {
        report("pipe-config", "invalid");
        return;
    }

    g_pipePath.fill(L'\0');
    std::copy(prefix.begin(), prefix.end(), g_pipePath.begin());
    std::copy(value.begin(), value.end(), g_pipePath.begin() + prefix.size());
}

[[nodiscard]] bool disabled_by_environment() noexcept {
    std::array<wchar_t, 16> value{};
    const DWORD length = GetEnvironmentVariableW(protocol::kDisabledEnvironment,
                                                  value.data(),
                                                  static_cast<DWORD>(value.size()));
    return length != 0 && value[0] != L'0';
}

