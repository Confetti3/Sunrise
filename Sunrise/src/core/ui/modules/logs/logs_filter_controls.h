#pragma once

#include "../../../logging/view/log_snapshot_view.h"

namespace sunrise::core::ui::modules::logs::internal {

void draw_channel_filter() noexcept;

void draw_level_filter() noexcept;

void draw_text_filter() noexcept;

[[nodiscard]] log::view::Filter current_filter() noexcept;

void reset_filters() noexcept;

} // namespace sunrise::core::ui::modules::logs::internal
