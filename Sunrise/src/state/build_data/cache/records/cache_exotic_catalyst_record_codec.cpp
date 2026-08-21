#include "codec.h"

namespace sunrise::state::build_data::cache::records {

bool encode(const items::catalysts::Definition& value, ExoticCatalystRecord& record) noexcept {
    record = {};
    if (!items::catalysts::valid_availability(value.availability)) {
        return false;
    }
    record.itemDefinitionHash = value.itemDefinitionHash;
    record.itemDefinitionIndex = value.itemDefinitionIndex;
    record.completedPlugDefinitionIndex = value.completedPlugDefinitionIndex;
    record.effectDefinitionIndex = value.effectDefinitionIndex;
    record.socketLane = value.socketLane;
    record.availability = static_cast<std::uint8_t>(value.availability);
    return true;
}

bool decode(const ExoticCatalystRecord& record, items::catalysts::Definition& value) noexcept {
    value = {};
    const auto availability = static_cast<items::catalysts::Availability>(record.availability);
    if (!items::catalysts::valid_availability(availability)) {
        return false;
    }
    value = {record.itemDefinitionHash,
             record.itemDefinitionIndex,
             record.completedPlugDefinitionIndex,
             record.effectDefinitionIndex,
             record.socketLane,
             availability};
    return true;
}

} // namespace sunrise::state::build_data::cache::records
