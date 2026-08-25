#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "../../inspection/inspection_scene.h"

namespace sunrise::client::ui::world_inspector {

namespace debug_scene {

using Vertex = inspection::SceneVertex;
using Line = inspection::SceneLine;
using DepthStatus = inspection::DepthRenderStatus;
inline constexpr std::size_t kMaximumLines = inspection::kMaximumSceneLines;

[[nodiscard]] bool enabled() noexcept;
void publish_status(const DepthStatus& status) noexcept;
[[nodiscard]] DepthStatus status() noexcept;
[[nodiscard]] inspection::SceneFramePtr frame() noexcept;

} // namespace debug_scene

void suspend() noexcept;
[[nodiscard]] bool visible() noexcept;
[[nodiscard]] bool render(bool uiVisible) noexcept;
void service_camera_path_captures() noexcept;

struct SelectionIdentity final {
    std::uint64_t producerEpoch{};
    std::uint64_t nativeKey{};
    std::uint32_t producer{};
    std::uint32_t kind{};
};

struct ReadinessSummary final {
    std::uint64_t declared{};
    std::uint64_t copied{};
    std::size_t producers{};
    std::size_t readyProducers{};
    bool truncated{};
};

struct EntrySnapshot final {
    ReadinessSummary runtime;
    ReadinessSummary authored;
    ReadinessSummary rendering;
    std::string packageName;
    std::string mapStem;
    std::uint64_t activitySession{};
    std::uint64_t valueRevision{};
    bool sessionPresent{};
    bool stale{};
};

/** Initializes render-thread workspace state. */
void initialize() noexcept;

/** Clears workspace, provider, and input ownership state. */
void shutdown() noexcept;

/** Opens the full-screen workspace on the next UI frame. */
void open() noexcept;

/** Draws the compact Sunrise module that enters the full-screen workspace. */
void draw_launcher() noexcept;

/** Returns to the ordinary Sunrise surface. */
void close() noexcept;

/** Refreshes and returns copied readiness for the ordinary Inspector entry page. */
[[nodiscard]] EntrySnapshot entry_snapshot() noexcept;

/** Copies the current graph-independent selection identity. */
[[nodiscard]] bool selected_identity(SelectionIdentity& identity) noexcept;

} // namespace sunrise::client::ui::world_inspector
