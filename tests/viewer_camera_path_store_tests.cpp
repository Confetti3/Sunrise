#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <limits>

#include "client/viewer/viewer_camera_path_store.h"
#include "core/logging/log.h"

namespace sunrise::core::log {
void write(Channel, Level, std::string_view) noexcept {}
} // namespace sunrise::core::log

namespace paths = sunrise::client::viewer::paths;

[[nodiscard]] bool require(bool condition, const char* message) {
    if (!condition) {
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }
    return condition;
}

int main() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!require(length != 0 && length < executable.size(), "executable path")) {
        return 1;
    }
    const std::filesystem::path artifact =
        std::filesystem::path(executable.data()).parent_path() / L"Sunrise";
    std::filesystem::create_directories(artifact);
    const std::filesystem::path file = artifact / L"viewer-paths.json";

    // A malformed persisted document is rejected wholly.
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << R"({"schema_version":1,"paths":[{"name":"broken"}]})";
    }
    paths::initialize(GetModuleHandleW(nullptr));
    if (!require(paths::get().paths.empty(), "malformed document rejection")) {
        return 2;
    }

    paths::Library library{};
    paths::CameraPath path{};
    path.name = "inspection route";
    paths::Keyframe first{};
    first.position = {1.0F, 2.0F, 3.0F};
    first.fov = 75.0F;
    first.label = "start";
    path.keyframes.push_back(first);
    library.paths.push_back(path);
    if (!require(paths::valid(library), "valid library")
        || !require(paths::publish(library), "atomic publish")
        || !require(paths::get() == library, "published copy")) {
        return 3;
    }

    paths::Library invalid = library;
    invalid.paths[0].keyframes[0].fov = std::numeric_limits<float>::quiet_NaN();
    if (!require(!paths::valid(invalid), "nonfinite rejection")) {
        return 4;
    }
    invalid = library;
    invalid.paths.push_back(path);
    if (!require(!paths::valid(invalid), "duplicate name rejection")) {
        return 5;
    }
    invalid = library;
    invalid.paths[0].keyframes.resize(paths::kMaximumKeyframeCount + 1);
    if (!require(!paths::valid(invalid), "keyframe capacity rejection")) {
        return 6;
    }

    paths::shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(artifact, ignored);
    return 0;
}
