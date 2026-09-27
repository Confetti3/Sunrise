#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "activity_sdk_lua_missions_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {

void append_int(std::string& output, std::int32_t value) {
    std::array<char, 16> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(), "%d", value);
    output.append(buffer.data(), static_cast<std::size_t>(length));
}

/** Appends the catalog-wide tables every mission module shares, in their published order. */
void append_catalog_constants(const Source& source, std::string& output) {
    std::string actorMessageConstants = "mission.ActorMessage = {\n";
    for (const format::ActorMessageSchema& row : source.actorMessageSchemas) {
        actorMessageConstants.append("    ");
        actorMessageConstants.append(identifier(text(source, row.name), true));
        actorMessageConstants.append(" = { definition = ");
        append_hex(actorMessageConstants, row.definitionHandle);
        actorMessageConstants.append(", durable_key = ");
        append_hex(actorMessageConstants, row.durableKey);
        actorMessageConstants.append(", owner_class = ");
        append_hex(actorMessageConstants, row.ownerClass);
        actorMessageConstants.append(", handler_slot = ");
        append_uint(actorMessageConstants, row.handlerSlot);
        actorMessageConstants.append(", body_type = ");
        append_uint(actorMessageConstants, row.bodyType);
        actorMessageConstants.append(" },\n");
    }
    actorMessageConstants.append("}\n");
    std::string actorCommandConstants = "mission.ActorCommand = {\n";
    std::string actorCommandDefinitions = "mission.ActorCommandDefinition = {\n";
    std::string factionConstants = "mission.Faction = {\n";
    bool factionValuesWritten = false;
    for (const format::ActorCommandDefinition& row : source.actorCommandDefinitions) {
        const std::string commandName = identifier(text(source, row.name), true);
        actorCommandConstants.append("    ");
        actorCommandConstants.append(commandName);
        actorCommandConstants.append(" = ");
        append_uint(actorCommandConstants, row.selector);
        actorCommandConstants.append(",\n");
        actorCommandDefinitions.append("    ");
        actorCommandDefinitions.append(commandName);
        actorCommandDefinitions.append(" = { selector = mission.ActorCommand.");
        actorCommandDefinitions.append(commandName);
        actorCommandDefinitions.append(", payload = ");
        append_hex(actorCommandDefinitions, row.payloadHandle);
        actorCommandDefinitions.append(" },\n");
        if (row.effect == format::ActorCommandEffect::setFaction && !factionValuesWritten) {
            factionConstants.append("    ");
            factionConstants.append(identifier(text(source, row.factionNoneName), true));
            factionConstants.append(" = ");
            append_int(factionConstants, row.factionNone);
            factionConstants.append(",\n    ");
            factionConstants.append(identifier(text(source, row.factionRemovedName), true));
            factionConstants.append(" = ");
            append_int(factionConstants, row.factionRemoved);
            factionConstants.append(",\n    ");
            factionConstants.append(identifier(text(source, row.factionHostileToAllName), true));
            factionConstants.append(" = ");
            append_int(factionConstants, row.factionHostileToAll);
            factionConstants.append(",\n");
            factionValuesWritten = true;
        }
    }
    actorCommandConstants.append("}\n");
    actorCommandDefinitions.append("}\n");
    factionConstants.append("}\n");
    std::string simulationEventConstants = "mission.SimulationEvent = {\n";
    std::string simulationEventDefinitions = "mission.SimulationEventDefinition = {\n";
    for (const format::SimulationEventDefinition& row : source.simulationEventDefinitions) {
        const std::string eventName = identifier(text(source, row.name), true);
        simulationEventConstants.append("    ");
        simulationEventConstants.append(eventName);
        simulationEventConstants.append(" = ");
        append_uint(simulationEventConstants, row.eventType);
        simulationEventConstants.append(",\n");
        simulationEventDefinitions.append("    ");
        simulationEventDefinitions.append(eventName);
        simulationEventDefinitions.append(" = { event_type = mission.SimulationEvent.");
        simulationEventDefinitions.append(eventName);
        simulationEventDefinitions.append(", primary_schema = ");
        if (row.primarySchema == format::kAbsentIndex) {
            simulationEventDefinitions.append("nil");
        } else {
            append_hex(simulationEventDefinitions, row.primarySchema);
        }
        simulationEventDefinitions.append(", secondary_schema = ");
        if (row.secondarySchema == format::kAbsentIndex) {
            simulationEventDefinitions.append("nil");
        } else {
            append_hex(simulationEventDefinitions, row.secondarySchema);
        }
        simulationEventDefinitions.append(" },\n");
    }
    simulationEventConstants.append("}\n");
    simulationEventDefinitions.append("}\n");
    std::string runtimeFieldTypes = "mission.RuntimeFieldType = {\n";
    // Lua name published for each runtime codec family.
    constexpr std::array<std::pair<format::RuntimeCodecFamily, std::string_view>, 3> kFamilies{{
        {format::RuntimeCodecFamily::activity, "ACTIVITY"},
        {format::RuntimeCodecFamily::sobjectModeZero, "SOBJECT_MODE_ZERO"},
        {format::RuntimeCodecFamily::sobjectModeOne, "SOBJECT_MODE_ONE"},
    }};
    for (const auto& [family, familyName] : kFamilies) {
        runtimeFieldTypes.append("    ");
        runtimeFieldTypes.append(familyName);
        runtimeFieldTypes.append(" = {\n");
        for (const format::RuntimeTypeDefinition& row : source.runtimeTypeDefinitions) {
            if ((row.codecFamilies & static_cast<std::uint32_t>(family)) == 0) {
                continue;
            }
            runtimeFieldTypes.append("        ");
            runtimeFieldTypes.append(identifier(text(source, row.name), true));
            runtimeFieldTypes.append(" = ");
            append_uint(runtimeFieldTypes, row.typeCode);
            runtimeFieldTypes.append(",\n");
        }
        runtimeFieldTypes.append("    },\n");
    }
    runtimeFieldTypes.append("}\n");
    output.append(actorMessageConstants);
    output.append(actorCommandConstants);
    output.append(actorCommandDefinitions);
    output.append(factionConstants);
    output.append(simulationEventConstants);
    output.append(simulationEventDefinitions);
    output.append(runtimeFieldTypes);
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
