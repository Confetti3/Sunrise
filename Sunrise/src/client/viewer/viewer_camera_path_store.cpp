#include "viewer_camera_path_store.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include "../../core/filesystem/path.h"
#include "../../core/filesystem/temporary_sibling.h"
#include "../../core/logging/log.h"

namespace sunrise::client::viewer::paths {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\viewer-paths.json";
constexpr std::size_t kMaximumFileBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumNumberBytes = 64;
constexpr unsigned kMaximumJsonDepth = 16;
constexpr unsigned kTemporaryAttempts = 8;

SRWLOCK g_lock{SRWLOCK_INIT};
Library g_library{};
core::path::Buffer g_path{};
bool g_pathResolved{};
volatile LONG g_temporarySequence{};

void report_fail(const char* reason) noexcept {
    std::array<char, 112> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=viewer_paths stage=store result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    return value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())
           && MultiByteToWideChar(CP_UTF8,
                                  MB_ERR_INVALID_CHARS,
                                  value.data(),
                                  static_cast<int>(value.size()),
                                  nullptr,
                                  0)
                  > 0;
}

[[nodiscard]] bool
valid_text(std::string_view value, std::size_t maximumBytes, bool allowEmpty) noexcept {
    return value.size() <= maximumBytes && (allowEmpty || !value.empty()) && valid_utf8(value)
           && std::ranges::none_of(value, [](unsigned char byte) { return byte < 0x20U; });
}

[[nodiscard]] bool valid_identity(const SelectionIdentity& identity) noexcept {
    return identity.nativeKey != 0;
}

[[nodiscard]] bool valid_keyframe(const Keyframe& keyframe) noexcept {
    return std::ranges::all_of(keyframe.position, [](float value) { return std::isfinite(value); })
           && std::isfinite(keyframe.yaw) && std::isfinite(keyframe.pitch)
           && keyframe.pitch >= -kMaximumPitch && keyframe.pitch <= kMaximumPitch
           && std::isfinite(keyframe.fov) && keyframe.fov >= kMinimumKeyframeFov
           && keyframe.fov <= kMaximumKeyframeFov && std::isfinite(keyframe.travelSeconds)
           && keyframe.travelSeconds >= 0.0F && keyframe.travelSeconds <= kMaximumSegmentSeconds
           && std::isfinite(keyframe.dwellSeconds) && keyframe.dwellSeconds >= 0.0F
           && keyframe.dwellSeconds <= kMaximumSegmentSeconds
           && valid_text(keyframe.label, kMaximumKeyframeLabelBytes, true)
           && (!keyframe.selection.has_value() || valid_identity(*keyframe.selection));
}

class Reader final {
public:
    explicit Reader(std::string_view input) noexcept : input_(input) {}

    [[nodiscard]] bool library(Library& output) {
        Library candidate{};
        bool sawVersion = false;
        bool sawPaths = false;
        if (!consume('{')) {
            return false;
        }
        if (consume('}')) {
            return false;
        }
        for (;;) {
            std::string name;
            if (!string(name) || !consume(':')) {
                return false;
            }
            if (name == "schema_version") {
                std::uint64_t version = 0;
                if (sawVersion || !unsigned_integer(version)
                    || version != static_cast<std::uint64_t>(kSchemaVersion)) {
                    return false;
                }
                candidate.schemaVersion = static_cast<std::uint32_t>(version);
                sawVersion = true;
            } else if (name == "paths") {
                if (sawPaths || !path_array(candidate.paths)) {
                    return false;
                }
                sawPaths = true;
            } else if (!skip_value(0)) {
                return false;
            }
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return false;
            }
        }
        if (!sawVersion || !sawPaths || !at_end() || !valid(candidate)) {
            return false;
        }
        output = std::move(candidate);
        return true;
    }

private:
    [[nodiscard]] bool path_array(std::vector<CameraPath>& output) {
        if (!consume('[')) {
            return false;
        }
        if (consume(']')) {
            return true;
        }
        for (;;) {
            if (output.size() >= kMaximumPathCount) {
                return false;
            }
            CameraPath path;
            if (!camera_path(path)) {
                return false;
            }
            output.push_back(std::move(path));
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool camera_path(CameraPath& output) {
        bool sawName = false;
        bool sawLoop = false;
        bool sawKeyframes = false;
        if (!consume('{') || consume('}')) {
            return false;
        }
        for (;;) {
            std::string name;
            if (!string(name) || !consume(':')) {
                return false;
            }
            if (name == "name") {
                if (sawName || !string(output.name)) {
                    return false;
                }
                sawName = true;
            } else if (name == "loop") {
                if (sawLoop || !boolean(output.loop)) {
                    return false;
                }
                sawLoop = true;
            } else if (name == "keyframes") {
                if (sawKeyframes || !keyframe_array(output.keyframes)) {
                    return false;
                }
                sawKeyframes = true;
            } else if (!skip_value(0)) {
                return false;
            }
            if (consume('}')) {
                return sawName && sawLoop && sawKeyframes;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool keyframe_array(std::vector<Keyframe>& output) {
        if (!consume('[')) {
            return false;
        }
        if (consume(']')) {
            return true;
        }
        for (;;) {
            if (output.size() >= kMaximumKeyframeCount) {
                return false;
            }
            Keyframe keyframe;
            if (!keyframe_value(keyframe)) {
                return false;
            }
            output.push_back(std::move(keyframe));
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool keyframe_value(Keyframe& output) {
        enum Field : unsigned {
            position = 1U << 0U,
            yaw = 1U << 1U,
            pitch = 1U << 2U,
            fov = 1U << 3U,
            travel = 1U << 4U,
            dwell = 1U << 5U,
            label = 1U << 6U,
            selection = 1U << 7U,
            capture = 1U << 8U,
        };
        constexpr unsigned required =
            position | yaw | pitch | fov | travel | dwell | label | selection | capture;
        unsigned fields = 0;
        if (!consume('{') || consume('}')) {
            return false;
        }
        for (;;) {
            std::string name;
            unsigned field = 0;
            bool parsed = false;
            if (!string(name) || !consume(':')) {
                return false;
            }
            if (name == "position") {
                field = position;
                parsed = vector3(output.position);
            } else if (name == "yaw") {
                field = yaw;
                parsed = floating_point(output.yaw);
            } else if (name == "pitch") {
                field = pitch;
                parsed = floating_point(output.pitch);
            } else if (name == "fov") {
                field = fov;
                parsed = floating_point(output.fov);
            } else if (name == "travel_seconds") {
                field = travel;
                parsed = floating_point(output.travelSeconds);
            } else if (name == "dwell_seconds") {
                field = dwell;
                parsed = floating_point(output.dwellSeconds);
            } else if (name == "label") {
                field = label;
                parsed = string(output.label);
            } else if (name == "selection") {
                field = selection;
                parsed = optional_identity(output.selection);
            } else if (name == "capture_snapshot") {
                field = capture;
                parsed = boolean(output.captureSnapshot);
            } else {
                parsed = skip_value(0);
            }
            if (!parsed || (field != 0 && (fields & field) != 0)) {
                return false;
            }
            fields |= field;
            if (consume('}')) {
                return fields == required && valid_keyframe(output);
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool optional_identity(std::optional<SelectionIdentity>& output) {
        if (literal("null")) {
            output.reset();
            return true;
        }
        SelectionIdentity candidate{};
        enum Field : unsigned {
            producer = 1U << 0U,
            epoch = 1U << 1U,
            key = 1U << 2U,
            kind = 1U << 3U,
        };
        constexpr unsigned required = producer | epoch | key | kind;
        unsigned fields = 0;
        if (!consume('{') || consume('}')) {
            return false;
        }
        for (;;) {
            std::string name;
            std::uint64_t value = 0;
            unsigned field = 0;
            if (!string(name) || !consume(':') || !unsigned_integer(value)) {
                return false;
            }
            if (name == "producer") {
                field = producer;
                if (value > (std::numeric_limits<std::uint32_t>::max)()) {
                    return false;
                }
                candidate.producer = static_cast<std::uint32_t>(value);
            } else if (name == "producer_epoch") {
                field = epoch;
                candidate.producerEpoch = value;
            } else if (name == "native_key") {
                field = key;
                candidate.nativeKey = value;
            } else if (name == "kind") {
                field = kind;
                if (value > (std::numeric_limits<std::uint32_t>::max)()) {
                    return false;
                }
                candidate.kind = static_cast<std::uint32_t>(value);
            } else {
                return false;
            }
            if ((fields & field) != 0) {
                return false;
            }
            fields |= field;
            if (consume('}')) {
                if (fields != required || !valid_identity(candidate)) {
                    return false;
                }
                output = candidate;
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool vector3(std::array<float, 3>& output) {
        return consume('[') && floating_point(output[0]) && consume(',')
               && floating_point(output[1]) && consume(',') && floating_point(output[2])
               && consume(']');
    }

    [[nodiscard]] bool skip_value(unsigned depth) {
        if (depth >= kMaximumJsonDepth) {
            return false;
        }
        whitespace();
        if (position_ >= input_.size()) {
            return false;
        }
        if (input_[position_] == '"') {
            std::string ignored;
            return string(ignored);
        }
        if (input_[position_] == '{') {
            ++position_;
            if (consume('}')) {
                return true;
            }
            for (;;) {
                std::string ignored;
                if (!string(ignored) || !consume(':') || !skip_value(depth + 1)) {
                    return false;
                }
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
            }
        }
        if (input_[position_] == '[') {
            ++position_;
            if (consume(']')) {
                return true;
            }
            for (;;) {
                if (!skip_value(depth + 1)) {
                    return false;
                }
                if (consume(']')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
            }
        }
        bool ignored = false;
        if (boolean(ignored) || literal("null")) {
            return true;
        }
        float number = 0.0F;
        return floating_point(number);
    }

    [[nodiscard]] bool string(std::string& output) {
        whitespace();
        if (position_ >= input_.size() || input_[position_++] != '"') {
            return false;
        }
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') {
                return valid_utf8(output);
            }
            if (value < 0x20U) {
                return false;
            }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escaped);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            default:
                // The store emits UTF-8 directly. Refusing \u avoids accepting surrogate halves
                // or silently changing an externally edited name.
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool boolean(bool& output) {
        if (literal("true")) {
            output = true;
            return true;
        }
        if (literal("false")) {
            output = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool unsigned_integer(std::uint64_t& output) {
        whitespace();
        const std::size_t begin = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (begin == position_ || (position_ - begin > 1 && input_[begin] == '0')) {
            position_ = begin;
            return false;
        }
        const auto converted =
            std::from_chars(input_.data() + begin, input_.data() + position_, output, 10);
        if (converted.ec != std::errc{} || converted.ptr != input_.data() + position_) {
            position_ = begin;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool floating_point(float& output) {
        whitespace();
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        if (position_ >= input_.size()) {
            position_ = begin;
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                position_ = begin;
                return false;
            }
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            do {
                ++position_;
            } while (position_ < input_.size() && input_[position_] >= '0'
                     && input_[position_] <= '9');
        } else {
            position_ = begin;
            return false;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction = position_;
            while (position_ < input_.size() && input_[position_] >= '0'
                   && input_[position_] <= '9') {
                ++position_;
            }
            if (fraction == position_) {
                position_ = begin;
                return false;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent = position_;
            while (position_ < input_.size() && input_[position_] >= '0'
                   && input_[position_] <= '9') {
                ++position_;
            }
            if (exponent == position_) {
                position_ = begin;
                return false;
            }
        }
        const std::size_t length = position_ - begin;
        if (length == 0 || length >= kMaximumNumberBytes) {
            position_ = begin;
            return false;
        }
        std::array<char, kMaximumNumberBytes> buffer{};
        std::ranges::copy(input_.data() + begin, input_.data() + position_, buffer.data());
        char* end = nullptr;
        const float value = std::strtof(buffer.data(), &end);
        if (end != buffer.data() + length || !std::isfinite(value)) {
            position_ = begin;
            return false;
        }
        output = value;
        return true;
    }

    [[nodiscard]] bool literal(std::string_view value) {
        whitespace();
        if (input_.substr(position_, value.size()) != value) {
            return false;
        }
        position_ += value.size();
        return true;
    }

    [[nodiscard]] bool consume(char value) {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != value) {
            return false;
        }
        ++position_;
        return true;
    }

    [[nodiscard]] bool at_end() {
        whitespace();
        return position_ == input_.size();
    }

    void whitespace() {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++position_;
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

void append_string(std::string& output, std::string_view value) {
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    output.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output.append("\\\"");
            break;
        case '\\':
            output.append("\\\\");
            break;
        case '\b':
            output.append("\\b");
            break;
        case '\f':
            output.append("\\f");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            if (byte < 0x20U) {
                output.append("\\u00");
                output.push_back(hexadecimal[byte >> 4U]);
                output.push_back(hexadecimal[byte & 0xFU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

template <typename Value> void append_integer(std::string& output, Value value) {
    std::array<char, 32> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
    if (converted.ec == std::errc{}) {
        output.append(text.data(), converted.ptr);
    }
}

void append_float(std::string& output, float value) {
    std::array<char, 32> text{};
    const auto converted = std::to_chars(text.data(),
                                         text.data() + text.size(),
                                         value,
                                         std::chars_format::general,
                                         std::numeric_limits<float>::max_digits10);
    if (converted.ec == std::errc{}) {
        output.append(text.data(), converted.ptr);
    }
}

[[nodiscard]] std::string serialize(const Library& library) {
    std::string output;
    output.reserve(256 + library.paths.size() * 256);
    output.append("{\n  \"schema_version\": ");
    append_integer(output, library.schemaVersion);
    output.append(",\n  \"paths\": [");
    for (std::size_t pathIndex = 0; pathIndex < library.paths.size(); ++pathIndex) {
        const CameraPath& path = library.paths[pathIndex];
        output.append(pathIndex == 0 ? "\n    {\n      \"name\": " : ",\n    {\n      \"name\": ");
        append_string(output, path.name);
        output.append(",\n      \"loop\": ");
        output.append(path.loop ? "true" : "false");
        output.append(",\n      \"keyframes\": [");
        for (std::size_t index = 0; index < path.keyframes.size(); ++index) {
            const Keyframe& keyframe = path.keyframes[index];
            output.append(index == 0 ? "\n        {\"position\": ["
                                     : ",\n        {\"position\": [");
            append_float(output, keyframe.position[0]);
            output.append(", ");
            append_float(output, keyframe.position[1]);
            output.append(", ");
            append_float(output, keyframe.position[2]);
            output.append("], \"yaw\": ");
            append_float(output, keyframe.yaw);
            output.append(", \"pitch\": ");
            append_float(output, keyframe.pitch);
            output.append(", \"fov\": ");
            append_float(output, keyframe.fov);
            output.append(", \"travel_seconds\": ");
            append_float(output, keyframe.travelSeconds);
            output.append(", \"dwell_seconds\": ");
            append_float(output, keyframe.dwellSeconds);
            output.append(", \"label\": ");
            append_string(output, keyframe.label);
            output.append(", \"selection\": ");
            if (!keyframe.selection.has_value()) {
                output.append("null");
            } else {
                const SelectionIdentity& identity = *keyframe.selection;
                output.append("{\"producer\": ");
                append_integer(output, identity.producer);
                output.append(", \"producer_epoch\": ");
                append_integer(output, identity.producerEpoch);
                output.append(", \"native_key\": ");
                append_integer(output, identity.nativeKey);
                output.append(", \"kind\": ");
                append_integer(output, identity.kind);
                output.push_back('}');
            }
            output.append(", \"capture_snapshot\": ");
            output.append(keyframe.captureSnapshot ? "true}" : "false}");
        }
        if (!path.keyframes.empty()) {
            output.push_back('\n');
            output.append("      ");
        }
        output.append("]\n    }");
    }
    if (!library.paths.empty()) {
        output.push_back('\n');
        output.append("  ");
    }
    output.append("]\n}\n");
    return output;
}

[[nodiscard]] bool write_atomic(std::string_view document) noexcept {
    if (!g_pathResolved || document.size() > kMaximumFileBytes
        || document.size() > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    core::path::Buffer temporary{};
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < kTemporaryAttempts && file == INVALID_HANDLE_VALUE;
         ++attempt) {
        temporary = g_path;
        std::array<wchar_t, 96> suffix{};
        const int written =
            std::swprintf(suffix.data(),
                          suffix.size(),
                          L".%08lX.%08lX.%08lX.tmp",
                          static_cast<unsigned long>(GetCurrentProcessId()),
                          static_cast<unsigned long>(GetCurrentThreadId()),
                          static_cast<unsigned long>(InterlockedIncrement(&g_temporarySequence)));
        if (written <= 0 || !core::path::append(temporary, suffix.data())) {
            return false;
        }
        file = CreateFileW(temporary.chars.data(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) {
            break;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, document.data(), static_cast<DWORD>(document.size()), &written, nullptr)
            != FALSE
        && written == static_cast<DWORD>(document.size()) && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    if (complete) {
        complete = MoveFileExW(temporary.chars.data(),
                               g_path.chars.data(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                   != FALSE;
    }
    if (!complete) {
        (void)DeleteFileW(temporary.chars.data());
    }
    return complete;
}

void load() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0
        || size.QuadPart > static_cast<LONGLONG>(kMaximumFileBytes)) {
        (void)CloseHandle(file);
        report_fail("size");
        return;
    }
    std::vector<char> document(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, document.data(), static_cast<DWORD>(document.size()), &read, nullptr)
            != FALSE
        && read == static_cast<DWORD>(document.size());
    (void)CloseHandle(file);
    if (!readOk) {
        report_fail("read");
        return;
    }
    std::string_view text(document.data(), document.size());
    constexpr std::string_view bom{"\xEF\xBB\xBF", 3};
    if (text.starts_with(bom)) {
        text.remove_prefix(bom.size());
    }
    Library parsed{};
    Reader reader(text);
    if (!reader.library(parsed)) {
        report_fail("parse");
        return;
    }
    g_library = std::move(parsed);
}

} // namespace

void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_library = Library{};
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    if (g_pathResolved) {
        core::path::remove_stale_siblings(g_path.chars.data());
        load();
    } else {
        report_fail("path");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_library = Library{};
    g_path = core::path::Buffer{};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

Library get() {
    AcquireSRWLockShared(&g_lock);
    const Library snapshot = g_library;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

bool valid(const Library& library) noexcept {
    if (library.schemaVersion != kSchemaVersion || library.paths.size() > kMaximumPathCount) {
        return false;
    }
    for (std::size_t index = 0; index < library.paths.size(); ++index) {
        const CameraPath& path = library.paths[index];
        if (!valid_text(path.name, kMaximumPathNameBytes, false)
            || path.keyframes.size() > kMaximumKeyframeCount
            || !std::ranges::all_of(path.keyframes, &valid_keyframe)) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (library.paths[prior].name == path.name) {
                return false;
            }
        }
    }
    return true;
}

bool publish(const Library& library) noexcept {
    if (!valid(library)) {
        report_fail("range");
        return false;
    }
    const std::string document = serialize(library);
    AcquireSRWLockExclusive(&g_lock);
    const bool stored = write_atomic(document);
    if (stored) {
        g_library = library;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        report_fail("write");
    }
    return stored;
}

} // namespace sunrise::client::viewer::paths
