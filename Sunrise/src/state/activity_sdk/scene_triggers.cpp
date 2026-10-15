#include "scene_triggers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../build_data/scriptables/inline_name_evidence.h"

namespace sunrise::state::activity_sdk {
namespace {

/** One known scene trigger name and the event key it hashes to. */
struct SceneEventName final {
    std::uint32_t key{};
    std::string_view name{};
};

/** @return The row for one trigger name; the engine stores its FNV-1 hash. */
[[nodiscard]] constexpr SceneEventName scene_event(std::string_view name) noexcept {
    return {build_data::scriptables::inline_name_evidence::hash(name), name};
}

// Scene trigger names. A scene graph keeps only each name's hash, so a name is matched by hash.
constexpr std::array kSceneEventNames{
    scene_event("1_trigger"),
    scene_event("2_trigger"),
    scene_event("a_path"),
    scene_event("a_path_complete"),
    scene_event("abort"),
    scene_event("activate_vex"),
    scene_event("adventurer_a_ready"),
    scene_event("adventurer_b_ready"),
    scene_event("adventurer_c_ready"),
    scene_event("alarm_off"),
    scene_event("alarm_on"),
    scene_event("alpha_strike_done"),
    scene_event("appledrop_complete"),
    scene_event("arrived"),
    scene_event("attack_left"),
    scene_event("attack_players"),
    scene_event("attack_right"),
    scene_event("b2r"),
    scene_event("b_despawn"),
    scene_event("b_finish"),
    scene_event("b_path"),
    scene_event("b_path_complete"),
    scene_event("b_wait_start"),
    scene_event("badge_turnin"),
    scene_event("basecamp_left"),
    scene_event("basecamp_right"),
    scene_event("boss_dead"),
    scene_event("boss_engaged"),
    scene_event("boss_intro_trigger"),
    scene_event("boss_loop_start"),
    scene_event("boss_move_done"),
    scene_event("boss_ready"),
    scene_event("boss_stunned"),
    scene_event("boss_teleport_done"),
    scene_event("boss_wipe_complete"),
    scene_event("c_path"),
    scene_event("c_path_complete"),
    scene_event("cabal"),
    scene_event("cancel_channel"),
    scene_event("cancel_scene"),
    scene_event("captain_dead"),
    scene_event("captain_engaged"),
    scene_event("captain_trigger"),
    scene_event("centurion_trigger"),
    scene_event("civilian_stop"),
    scene_event("close_door"),
    scene_event("complete"),
    scene_event("conflux_complete"),
    scene_event("container_trigger"),
    scene_event("crota_engaged"),
    scene_event("d_path"),
    scene_event("d_path_complete"),
    scene_event("dead"),
    scene_event("death_complete_trigger"),
    scene_event("death_scene_end"),
    scene_event("dialog_finished"),
    scene_event("dialogue_started_trigger"),
    scene_event("dog_trigger"),
    scene_event("draw_trigger"),
    scene_event("dummy"),
    scene_event("end_anim"),
    scene_event("end_anim2"),
    scene_event("end_anim3"),
    scene_event("end_anim4"),
    scene_event("end_anim5"),
    scene_event("end_finale"),
    scene_event("end_intro"),
    scene_event("end_loop"),
    scene_event("end_stunned"),
    scene_event("end_trigger"),
    scene_event("end_wipe"),
    scene_event("engaged_trigger"),
    scene_event("entry_trigger"),
    scene_event("exit"),
    scene_event("exit_closet"),
    scene_event("exit_loop"),
    scene_event("explosion_trigger"),
    scene_event("eye_attack"),
    scene_event("eye_open"),
    scene_event("failure"),
    scene_event("fallen"),
    scene_event("fallen_start"),
    scene_event("first_patrol"),
    scene_event("forever"),
    scene_event("forsaken"),
    scene_event("frame_die"),
    scene_event("gambit_base"),
    scene_event("go"),
    scene_event("go_trigger"),
    scene_event("h10_complete"),
    scene_event("h10_start"),
    scene_event("h11_complete"),
    scene_event("h11_start"),
    scene_event("h12_complete"),
    scene_event("h12_start"),
    scene_event("h13_complete"),
    scene_event("h13_start"),
    scene_event("h14_complete"),
    scene_event("h14_start"),
    scene_event("hive"),
    scene_event("howl_begin"),
    scene_event("howl_point0"),
    scene_event("howl_point1"),
    scene_event("howl_point2"),
    scene_event("howl_point3"),
    scene_event("howl_point4"),
    scene_event("howl_point5"),
    scene_event("howl_point6"),
    scene_event("howl_point7"),
    scene_event("howl_point8"),
    scene_event("howl_point9"),
    scene_event("idle_done"),
    scene_event("idle_exit_trigger"),
    scene_event("interact_approach_trigger"),
    scene_event("interrupt"),
    scene_event("interrupt_death"),
    scene_event("intro_done"),
    scene_event("kill"),
    scene_event("kill_boss"),
    scene_event("kill_fx"),
    scene_event("kill_turret"),
    scene_event("kneel"),
    scene_event("l2b"),
    scene_event("l2r"),
    scene_event("lens_destroyed"),
    scene_event("look"),
    scene_event("loop"),
    scene_event("loop_done"),
    scene_event("match_point"),
    scene_event("minion_finished"),
    scene_event("minion_work_done"),
    scene_event("minotaur"),
    scene_event("minotaur_trigger"),
    scene_event("mission_complete"),
    scene_event("move_march"),
    scene_event("no_shield_trigger"),
    scene_event("obj_spawned"),
    scene_event("outro_trigger"),
    scene_event("panoptes_spawn"),
    scene_event("past_scene_done"),
    scene_event("patrol_end"),
    scene_event("plate_done"),
    scene_event("platform_anchor_dead"),
    scene_event("player_arriving"),
    scene_event("player_enters"),
    scene_event("player_exits"),
    scene_event("player_near"),
    scene_event("player_spawned"),
    scene_event("player_trigger"),
    scene_event("pt_flare"),
    scene_event("r2b"),
    scene_event("ritual_trigger"),
    scene_event("rush_done"),
    scene_event("rush_ready"),
    scene_event("saint_approached"),
    scene_event("saint_attack"),
    scene_event("saint_outro"),
    scene_event("saint_outro_done"),
    scene_event("scene_finished"),
    scene_event("shield_down_trigger"),
    scene_event("shield_trigger"),
    scene_event("shield_up_trigger"),
    scene_event("spawn"),
    scene_event("spawn_turrets"),
    scene_event("spawn_turrets2"),
    scene_event("spawn_turrets3"),
    scene_event("spawn_turrets4"),
    scene_event("spawn_turrets5"),
    scene_event("squad_dead"),
    scene_event("start_anim"),
    scene_event("start_march"),
    scene_event("start_slam"),
    scene_event("start_trigger"),
    scene_event("start_vignette"),
    scene_event("stop_anim"),
    scene_event("stop_loop"),
    scene_event("stop_ritual"),
    scene_event("success"),
    scene_event("t_activate_vex"),
    scene_event("t_bomb_drop"),
    scene_event("t_boss_dead"),
    scene_event("t_boss_spawned"),
    scene_event("t_bubble_1_done"),
    scene_event("t_bubble_2_done"),
    scene_event("t_cabal_5_and_6_dead"),
    scene_event("t_cabal_7_and_8_dead"),
    scene_event("t_cabal_dropoffs_done"),
    scene_event("t_cabal_runups_attack"),
    scene_event("t_cabal_runups_done"),
    scene_event("t_cabal_runups_move"),
    scene_event("t_cayde_comment"),
    scene_event("t_civ_stop"),
    scene_event("t_civilian_dead"),
    scene_event("t_civilian_stop"),
    scene_event("t_combat_complete"),
    scene_event("t_combat_start"),
    scene_event("t_combat_start_+3"),
    scene_event("t_complete"),
    scene_event("t_corsair_talk"),
    scene_event("t_cyclops_squad_dead"),
    scene_event("t_despawn_fanatic"),
    scene_event("t_despawn_osiris"),
    scene_event("t_despawn_petra"),
    scene_event("t_dialogue_complete"),
    scene_event("t_dialogue_done"),
    scene_event("t_die"),
    scene_event("t_dregs_10_spawn"),
    scene_event("t_dregs_4_spawn"),
    scene_event("t_dropoff_1_move"),
    scene_event("t_dropoff_2_move"),
    scene_event("t_dropoff_3_move"),
    scene_event("t_dropoff_4_move"),
    scene_event("t_echo_five"),
    scene_event("t_echo_four"),
    scene_event("t_echo_one"),
    scene_event("t_echo_six"),
    scene_event("t_echo_start"),
    scene_event("t_echo_three"),
    scene_event("t_echo_two"),
    scene_event("t_end"),
    scene_event("t_end_celebration"),
    scene_event("t_end_mission"),
    scene_event("t_end_scene"),
    scene_event("t_end_scene_start_hop_on"),
    scene_event("t_eris_interact_1"),
    scene_event("t_eris_interact_2"),
    scene_event("t_eris_interact_3"),
    scene_event("t_explode_mosquito"),
    scene_event("t_explode_wall"),
    scene_event("t_facetime_vo_complete"),
    scene_event("t_falcon_fly"),
    scene_event("t_finish_intro"),
    scene_event("t_flee"),
    scene_event("t_flee_2"),
    scene_event("t_forever"),
    scene_event("t_frames_die"),
    scene_event("t_frames_shoot"),
    scene_event("t_ghaul_line1"),
    scene_event("t_ghaul_line2"),
    scene_event("t_ghost_exit_1"),
    scene_event("t_ghost_exit_2"),
    scene_event("t_ghost_exit_3"),
    scene_event("t_ghost_line1"),
    scene_event("t_ghost_line2"),
    scene_event("t_if_entered"),
    scene_event("t_intro"),
    scene_event("t_kill_cabal"),
    scene_event("t_kill_frame"),
    scene_event("t_kill_timeline"),
    scene_event("t_kill_vex"),
    scene_event("t_kneel"),
    scene_event("t_knight_wake"),
    scene_event("t_leave_cover"),
    scene_event("t_look"),
    scene_event("t_marauder_move"),
    scene_event("t_mistake_start"),
    scene_event("t_move_echo"),
    scene_event("t_omg_gogogo"),
    scene_event("t_player_approach"),
    scene_event("t_player_approaching"),
    scene_event("t_player_enter"),
    scene_event("t_preboss_complete"),
    scene_event("t_pride_rock"),
    scene_event("t_random_bunker_delay"),
    scene_event("t_react"),
    scene_event("t_shield_1"),
    scene_event("t_shoot"),
    scene_event("t_spawn_boss"),
    scene_event("t_spawn_chest"),
    scene_event("t_spawn_dreg"),
    scene_event("t_spawn_ghaul"),
    scene_event("t_spawn_vex"),
    scene_event("t_start"),
    scene_event("t_start_cabal"),
    scene_event("t_start_celebration"),
    scene_event("t_start_dialogue"),
    scene_event("t_start_fanatic_spawn"),
    scene_event("t_start_intro"),
    scene_event("t_start_scene"),
    scene_event("t_start_taunt"),
    scene_event("t_start_vignette"),
    scene_event("t_stop"),
    scene_event("t_stop_boss_attack"),
    scene_event("t_stop_hopon"),
    scene_event("t_stop_scene"),
    scene_event("t_stop_taunt"),
    scene_event("t_taunt_finished"),
    scene_event("t_tech_witch_talk"),
    scene_event("t_undertaker_dead"),
    scene_event("t_wretch_jump"),
    scene_event("t_zavala_speak"),
    scene_event("taunt_complete"),
    scene_event("team_ready"),
    scene_event("teleport"),
    scene_event("teleport_complete"),
    scene_event("teleport_switch"),
    scene_event("teleport_trigger"),
    scene_event("test"),
    scene_event("tower_1_complete"),
    scene_event("tower_2_complete"),
    scene_event("tr_explode"),
    scene_event("tr_kill_mosquito_1"),
    scene_event("tr_kill_mosquito_2"),
    scene_event("tr_player_enter"),
    scene_event("tr_spawn"),
    scene_event("trig_looters_retreat"),
    scene_event("trig_phase_exit"),
    scene_event("trig_tank_walk"),
    scene_event("trig_vexbro_look"),
    scene_event("trigger_all"),
    scene_event("trigger_closed_interrupt_0"),
    scene_event("trigger_end"),
    scene_event("trigger_fake_death"),
    scene_event("trigger_stop_loop"),
    scene_event("turret1"),
    scene_event("turret2"),
    scene_event("turret_move"),
    scene_event("turret_trigger"),
    scene_event("vex"),
    scene_event("vexbro_despawn"),
    scene_event("vexbro_goodbye"),
    scene_event("vexbro_move"),
    scene_event("vexbro_move_finished"),
    scene_event("wait_trigger"),
    scene_event("warbeast_dead"),
    scene_event("welcome_player_enter"),
    scene_event("zavala_shield_1"),
    scene_event("zavala_shield_2"),
    scene_event("zavala_start"),
    scene_event("zavala_wave_1"),
};

/** @return True when no two trigger names share a hash. */
[[nodiscard]] consteval bool distinct_keys() {
    std::array<std::uint32_t, kSceneEventNames.size()> keys{};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        keys[index] = kSceneEventNames[index].key;
    }
    std::sort(keys.begin(), keys.end());
    return std::adjacent_find(keys.begin(), keys.end()) == keys.end();
}

// A key names its first matching row, so a shared hash would hide a name.
static_assert(distinct_keys(), "two scene trigger names share a hash");

} // namespace

/** @return The known trigger name for a scene event key, or empty. */
std::string_view scene_trigger_name(std::uint32_t key) noexcept {
    for (const SceneEventName& row : kSceneEventNames) {
        if (row.key == key) {
            return row.name;
        }
    }
    return {};
}

/**
 * Finds one scene's gate rows by binary search.
 * @param gates Every gate row, sorted by scene slot row.
 * @param slotRow Slot row of the scene.
 * @return The scene's rows; empty when it has none.
 */
std::span<const format::AuthoredSceneEventKey>
scene_gates(std::span<const format::AuthoredSceneEventKey> gates, std::uint32_t slotRow) noexcept {
    const auto first = std::lower_bound(
        gates.begin(), gates.end(), slotRow, [](const auto& row, std::uint32_t slot) {
            return row.sceneSlotIndex < slot;
        });
    const auto last =
        std::upper_bound(first, gates.end(), slotRow, [](std::uint32_t slot, const auto& row) {
            return slot < row.sceneSlotIndex;
        });
    return {first, last};
}

bool scene_listens(std::span<const format::AuthoredSceneEventKey> gates,
                   std::uint32_t slotRow,
                   std::uint32_t key) noexcept {
    const auto scene = scene_gates(gates, slotRow);
    return std::any_of(scene.begin(), scene.end(), [key](const auto& gate) {
        return gate.key == key && (gate.flags & format::kAuthoredSceneEventKeyListened) != 0;
    });
}

/** @return True when no graph block fires the gate but at least one waits on it. */
bool scene_trigger_is_input(const format::AuthoredSceneEventKey& gate) noexcept {
    return gate.ordinal >= 0 && (gate.flags & format::kAuthoredSceneEventKeyListened) != 0
           && (gate.flags & format::kAuthoredSceneEventKeyFired) == 0;
}

/** @return True when a graph block fires the gate and none waits on it. */
bool scene_trigger_is_output(const format::AuthoredSceneEventKey& gate) noexcept {
    return (gate.flags & format::kAuthoredSceneEventKeyFired) != 0
           && (gate.flags & format::kAuthoredSceneEventKeyListened) == 0;
}

} // namespace sunrise::state::activity_sdk
