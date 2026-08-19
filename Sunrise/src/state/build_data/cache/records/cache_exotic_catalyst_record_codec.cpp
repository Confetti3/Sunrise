#include "codec.h"

namespace sunrise::state::build_data::cache::records {

bool encode(const items::catalysts::Definition& value,
            ExoticCatalystRecord& record) noexcept {
    record = {};
    if (value.reserved != 0
        || (value.availability != items::catalysts::Availability::released
            && value.availability != items::catalysts::Availability::placeholder
            && value.availability != items::catalysts::Availability::unsupported)) {
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

bool decode(const ExoticCatalystRecord& record,
            items::catalysts::Definition& value) noexcept {
    value = {};
    const auto availability = static_cast<items::catalysts::Availability>(record.availability);
    if (record.reserved != 0
        || (availability != items::catalysts::Availability::released
            && availability != items::catalysts::Availability::placeholder
            && availability != items::catalysts::Availability::unsupported)) {
        return false;
    }
    value = {record.itemDefinitionHash,
             record.itemDefinitionIndex,
             record.completedPlugDefinitionIndex,
             record.effectDefinitionIndex,
             record.socketLane,
             availability,
             0};
    return true;
}

} // namespace sunrise::state::build_data::cache::records
