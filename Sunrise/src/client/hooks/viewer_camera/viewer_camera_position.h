#pragma once

#include <cmath>

#include "viewer_camera.h"

namespace sunrise::client::viewer::camera::position {

/**
 * Detached camera coordinates are intentionally not restricted to the local
 * player zone. Only non-finite values are rejected because they can corrupt
 * camera matrices and downstream renderer state.
 * @param value World-space position to validate.
 * @return True when all vector lanes are finite.
 */
[[nodiscard]] inline bool valid(const Vector& value) noexcept {
    for (const float lane : value) {
        if (!std::isfinite(lane)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::client::viewer::camera::position
