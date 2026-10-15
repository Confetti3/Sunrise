#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

#include "../../../state/activity_sdk/scene_triggers.h"
#include "activity_sdk_lua_missions_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {
namespace {

/**
 * Appends the gates one predicate selects as constants, named where the trigger name is known.
 * @return The number of gates appended.
 */
std::size_t append_gates(std::span<const format::AuthoredSceneEventKey> gates,
                         bool (*selected)(const format::AuthoredSceneEventKey&) noexcept,
                         std::string& output) {
    std::unordered_set<std::string> names;
    std::size_t appended = 0;
    for (const format::AuthoredSceneEventKey& gate : gates) {
        if (!selected(gate)) {
            continue;
        }
        ++appended;
        const std::string_view known = state::activity_sdk::scene_trigger_name(gate.key);
        std::string name = known.empty() ? std::string{} : identifier(known, true);
        // An unknown name, or one that folds onto an earlier name, takes the key itself.
        if (name.empty() || !names.insert(name).second) {
            std::array<char, 16> symbol{};
            const int length = std::snprintf(symbol.data(), symbol.size(), "KEY_%08X", gate.key);
            name.assign(symbol.data(), static_cast<std::size_t>(length));
        }
        output.append("        ");
        output.append(name);
        output.append(" = ");
        append_hex(output, gate.key);
        output.append(",\n");
    }
    return appended;
}

/** Appends one scene's block to a table when the predicate selects any of its gates. */
void append_scene_table(std::string_view scene,
                        std::span<const format::AuthoredSceneEventKey> gates,
                        bool (*selected)(const format::AuthoredSceneEventKey&) noexcept,
                        std::string& output) {
    std::string block;
    if (append_gates(gates, selected, block) == 0) {
        return;
    }
    output.append("    ").append(scene).append(" = {\n").append(block).append("    },\n");
}

} // namespace

/** Appends one scene's inputs and outputs to their constant tables. */
void append_scene_ports(std::string_view scene,
                        std::span<const format::AuthoredSceneEventKey> gates,
                        std::string& inputs,
                        std::string& outputs) {
    append_scene_table(scene, gates, &state::activity_sdk::scene_trigger_is_input, inputs);
    append_scene_table(scene, gates, &state::activity_sdk::scene_trigger_is_output, outputs);
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
