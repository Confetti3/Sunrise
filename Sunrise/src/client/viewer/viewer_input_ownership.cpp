#include "viewer_input_ownership.h"

#include <atomic>

namespace sunrise::client::viewer::input {
namespace {

std::atomic_bool g_workspaceNavigation{};

} // namespace

void set_workspace_navigation(bool active) noexcept {
    g_workspaceNavigation.store(active, std::memory_order_release);
}

bool workspace_navigation() noexcept {
    return g_workspaceNavigation.load(std::memory_order_acquire);
}

void reset() noexcept {
    set_workspace_navigation(false);
}

} // namespace sunrise::client::viewer::input
