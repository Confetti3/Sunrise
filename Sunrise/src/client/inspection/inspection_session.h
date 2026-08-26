#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "inspection_capture.h"
#include "inspection_scene.h"
#include "providers/spawn_inspection_provider.h"

namespace sunrise::client::inspection {

/** Owns the current copied document and non-visual inspection session state. */
class InspectionSession final {
public:
    [[nodiscard]] providers::RefreshResult refresh();
    void reset() noexcept;
    void reset_document() noexcept;

    [[nodiscard]] const InspectionDocument& document() const noexcept;
    [[nodiscard]] capture::History& history() noexcept;
    [[nodiscard]] const capture::History& history() const noexcept;

    void capture_comparison_baseline();
    void compare_with_baseline();
    void clear_comparison() noexcept;
    [[nodiscard]] const std::optional<capture::InspectionSnapshot>&
    comparison_baseline() const noexcept;
    [[nodiscard]] const std::vector<capture::ChangeEvent>& comparison_events() const noexcept;

    void export_json() noexcept;
    void export_csv() noexcept;
    void export_events() noexcept;
    void export_route(const capture::RouteCaptureMetadata& metadata) noexcept;
    [[nodiscard]] const capture::ExportResult& last_export() const noexcept;

    void publish_overlay(SceneFrame frame) noexcept;
    void clear_overlay() noexcept;
    [[nodiscard]] SceneFramePtr overlay_frame() const noexcept;
    void publish_depth_status(const DepthRenderStatus& status) noexcept;
    [[nodiscard]] DepthRenderStatus depth_status() const noexcept;

private:
    providers::SpawnInspectionProvider provider_;
    capture::History history_;
    std::optional<capture::InspectionSnapshot> comparisonBaseline_;
    std::vector<capture::ChangeEvent> comparisonEvents_;
    capture::ExportResult lastExport_{};
};

} // namespace sunrise::client::inspection
