#include "inspection_session.h"

#include <atomic>
#include <memory>
#include <utility>

namespace sunrise::client::inspection {
namespace {

const SceneFramePtr g_emptyScene = std::make_shared<const SceneFrame>();
const std::shared_ptr<const DepthRenderStatus> g_emptyStatus =
    std::make_shared<const DepthRenderStatus>();
std::atomic<SceneFramePtr> g_sceneFrame{g_emptyScene};
std::atomic<std::shared_ptr<const DepthRenderStatus>> g_renderStatus{g_emptyStatus};

} // namespace

providers::RefreshResult InspectionSession::refresh() {
    const providers::RefreshResult result = provider_.refresh();
    history_.observe(provider_.snapshot());
    return result;
}

void InspectionSession::set_live_runtime_membership(bool enabled) noexcept {
    provider_.set_live_runtime_membership(enabled);
}

void InspectionSession::reset() noexcept {
    provider_.reset();
    history_.clear();
    clear_comparison();
    lastExport_ = {};
    clear_overlay();
}

void InspectionSession::reset_document() noexcept {
    provider_.reset();
}

const InspectionDocument& InspectionSession::document() const noexcept {
    return provider_.snapshot();
}

capture::History& InspectionSession::history() noexcept {
    return history_;
}

const capture::History& InspectionSession::history() const noexcept {
    return history_;
}

void InspectionSession::capture_comparison_baseline() {
    comparisonBaseline_ = capture::make_snapshot(document());
    comparisonEvents_.clear();
}

void InspectionSession::compare_with_baseline() {
    if (comparisonBaseline_.has_value()) {
        comparisonEvents_ =
            capture::compare(*comparisonBaseline_, capture::make_snapshot(document()));
    }
}

void InspectionSession::clear_comparison() noexcept {
    comparisonBaseline_.reset();
    comparisonEvents_.clear();
}

const std::optional<capture::InspectionSnapshot>&
InspectionSession::comparison_baseline() const noexcept {
    return comparisonBaseline_;
}

const std::vector<capture::ChangeEvent>& InspectionSession::comparison_events() const noexcept {
    return comparisonEvents_;
}

void InspectionSession::export_json() noexcept {
    lastExport_ = capture::export_json(capture::make_snapshot(document()));
}

void InspectionSession::export_csv() noexcept {
    lastExport_ = capture::export_csv(capture::make_snapshot(document()));
}

void InspectionSession::export_events() noexcept {
    lastExport_ = capture::export_events(history_.events(), capture::make_snapshot(document()));
}

void InspectionSession::export_route(const capture::RouteCaptureMetadata& metadata) noexcept {
    lastExport_ = capture::export_route_json(capture::make_snapshot(document()), metadata);
}

const capture::ExportResult& InspectionSession::last_export() const noexcept {
    return lastExport_;
}

void InspectionSession::publish_overlay(SceneFrame frame) noexcept {
    try {
        g_sceneFrame.store(std::make_shared<const SceneFrame>(std::move(frame)),
                           std::memory_order_release);
    } catch (...) {
        // Keep the previous immutable batch on allocation failure.
    }
}

void InspectionSession::clear_overlay() noexcept {
    g_sceneFrame.store(g_emptyScene, std::memory_order_release);
    g_renderStatus.store(g_emptyStatus, std::memory_order_release);
}

SceneFramePtr InspectionSession::overlay_frame() const noexcept {
    return g_sceneFrame.load(std::memory_order_acquire);
}

void InspectionSession::publish_depth_status(const DepthRenderStatus& status) noexcept {
    try {
        g_renderStatus.store(std::make_shared<const DepthRenderStatus>(status),
                             std::memory_order_release);
    } catch (...) {
        // Diagnostics are best-effort; never disturb rendering on allocation failure.
    }
}

DepthRenderStatus InspectionSession::depth_status() const noexcept {
    const std::shared_ptr<const DepthRenderStatus> status =
        g_renderStatus.load(std::memory_order_acquire);
    return status ? *status : DepthRenderStatus{};
}

} // namespace sunrise::client::inspection
