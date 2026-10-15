/** Skips the roster delta a sense update mirrors back, so the groups after it can be read. */

#include <cstddef>
#include <cstdint>

#include "sense_roster_delta.h"
#include "sensor_auth_update.h"

namespace sunrise::middleware::bap::activity_message::sense_update {
namespace {
namespace bits = middleware::encoding::bits;
namespace auth = sensor_auth_update;

/** Width of one biased state byte. */
constexpr std::size_t kStateWidth = 8;
/** Mask words are 32 bits wide. */
constexpr std::size_t kMaskWordWidth = 32;

/** @return False when the presence bit is missing; present receives it. */
[[nodiscard]] bool read_presence(bits::Reader& reader, bool& present) noexcept {
    std::uint64_t bit = 0;
    if (!reader.read(1, bit)) {
        return false;
    }
    present = bit != 0;
    return true;
}
/** Skips an optional counted array: presence, count, then that many fixed-width elements. */
[[nodiscard]] bool skip_counted(bits::Reader& reader,
                                std::uint8_t countWidth,
                                std::uint64_t capacity,
                                std::size_t elementWidth) noexcept {
    bool present = false;
    std::uint64_t count = 0;
    return read_presence(reader, present)
           && (!present
               || (reader.read(countWidth, count) && count <= capacity
                   && reader.skip(static_cast<std::size_t>(count) * elementWidth)));
}
/** Skips an optional fixed-width field: presence, then the field when it is set. */
[[nodiscard]] bool skip_fixed(bits::Reader& reader, std::size_t width) noexcept {
    bool present = false;
    return read_presence(reader, present) && (!present || reader.skip(width));
}
/** Skips the keys, mask and state bytes of one half; both halves share this shape. */
[[nodiscard]] bool skip_keyed(bits::Reader& reader,
                              std::uint8_t countWidth,
                              std::uint64_t capacity,
                              std::size_t maskBits) noexcept {
    return skip_counted(reader, countWidth, capacity, auth::kKeyWidth)
           && skip_fixed(reader, maskBits)
           && skip_counted(reader, countWidth, capacity, kStateWidth);
}

} // namespace

/** Every nested field has its own presence bit, so an unchanged field is one clear bit. */
bool skip_roster_delta(bits::Reader& reader) noexcept {
    bool keyed = false, bubbles = false;
    if (!read_presence(reader, keyed)
        || (keyed
            && !skip_keyed(reader,
                           auth::kDeltaCountWidth,
                           auth::kTopLevelGroupCapacity,
                           kMaskWordWidth * auth::kDeltaMaskWords))
        || !read_presence(reader, bubbles)) {
        return false;
    }
    std::uint64_t count = 0;
    if (bubbles
        && (!reader.read(auth::kBubbleCountWidth, count)
            || count > auth::kBubbleSubBlockCapacity)) {
        return false;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        bool nested = false;
        if (!skip_fixed(reader, auth::kKeyWidth) || !read_presence(reader, nested)
            || (nested
                && !skip_keyed(reader,
                               auth::kBubbleCountWidth,
                               auth::kBubbleKeyCapacity,
                               kMaskWordWidth * auth::kBubbleMaskWords))) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::sense_update
