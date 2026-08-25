#include "activity_override_enrichment.h"

#include <algorithm>
#include <string_view>

#include "../../../client/inspection/providers/activity_logic_inspection.h"

namespace sunrise::server::ui::activity_override::enrichment {
namespace {

template <std::size_t Capacity>
void copy_text(std::string_view source,
               std::array<char, Capacity>& destination,
               std::uint8_t& length) noexcept {
    const std::size_t copied = (std::min)(source.size(), destination.size() - 1);
    std::copy_n(source.begin(), copied, destination.begin());
    length = static_cast<std::uint8_t>(copied);
}

} // namespace

void resolve(const state::build_data::worlds::Summary& world, Summary& output) noexcept {
    output = {};
    namespace activity = client::inspection::providers::activity_logic;
    const activity::BrowseSummary* authored = activity::find_browse_activity(world.scenarioTag);
    if (authored != nullptr) {
        output.activityPresent = true;
        output.activityBuildMatch = activity::compatible();
        copy_text(authored->activityName, output.activityName, output.activityNameLength);
        copy_text(authored->destination, output.destination, output.destinationLength);
    }
}

} // namespace sunrise::server::ui::activity_override::enrichment
