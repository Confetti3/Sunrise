#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "server/gameplay/mission/mission_compiler.h"

using namespace sunrise::server::gameplay::mission;

namespace {

constexpr const char* kValid = R"(
return mission {
  id="test", version=1, destination="edz_freeroam",
  budgets={triggers=1, objectives=1, waves=1, content_steps=1, transitions=1},
  objectives={{id="objective", initial="active"}},
  interactions={{id="volume", bubble=56, position={x=408.104538,y=406.008057,z=78.609955}, extents={x=75,y=60,z=10}}},
  waves={{id="wave", spawner=0x80C26B0A, mode=0, requested={1,0}}},
  content_steps={{id="glimmer_intro"}},
  transitions={{event="trigger_enter", source="volume", actions={
    {type="change_objective", target="objective", value=2},
    {type="activate_content_step", target="glimmer_intro"},
    {type="activate_wave", target="wave"}
  }}}
})";

constexpr const char* kSignalsAndTimers = R"(
return mission {
  id="timed", version=1, destination="edz_freeroam",
  budgets={triggers=1, objectives=1, waves=1, content_steps=1,
           content_signals=1, timers=1, transitions=2},
  objectives={{id="objective", initial="active"}},
  interactions={{id="volume", bubble=56, position={x=408.104538,y=406.008057,z=78.609955}, extents={x=75,y=60,z=10}}},
  waves={{id="wave", spawner=0x80C26B0A, mode=0, requested={1,0}}},
  content_steps={{id="glimmer_intro"}},
  content_signals={{id="site0_spawn"}},
  timers={{id="site0_timeout", delay=30}},
  transitions={
    {event="content_signal", source="site0_spawn", actions={{type="schedule_timer", target="site0_timeout"}}},
    {event="timer_fired", source="site0_timeout", actions={{type="change_mission_state", value=2}}}
  }
})";

std::string replace(std::string source, const std::string& from, const std::string& to) {
    const std::size_t position = source.find(from);
    assert(position != std::string::npos);
    source.replace(position, from.size(), to);
    return source;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::filesystem::path missionPath =
        std::filesystem::path{argv[1]} / "Sunrise/missions/edz_freeroam.lua";
    std::ifstream missionFile(missionPath, std::ios::binary);
    assert(missionFile.good());
    const std::string trostland{std::istreambuf_iterator<char>(missionFile), {}};
    const MissionCompileResult trostlandResult =
        compile_mission_source(trostland, "edz_freeroam");
    assert(trostlandResult.status == MissionCompileStatus::success);
    assert(trostlandResult.program.version == 5);
    assert(trostlandResult.program.interactions[0].bubble == 51);
    assert(trostlandResult.program.contentStepCount == 10);
    assert(trostlandResult.program.timerCount == 12);
    assert(trostlandResult.program.timers[0].delayTicks == 900);

    const ContentStep* introStep = nullptr;
    const ContentStep* site0EnterStep = nullptr;
    bool unsupportedStepDeclared = false;
    for (std::size_t index = 0; index < trostlandResult.program.contentStepCount; ++index) {
        const ContentStep& step = trostlandResult.program.contentSteps[index];
        if (step.kind == ContentStepKind::glimmerIntro) introStep = &step;
        if (step.kind == ContentStepKind::glimmerSite0Enter) site0EnterStep = &step;
        unsupportedStepDeclared = unsupportedStepDeclared
                                  || step.kind == ContentStepKind::glimmerSite0Crew
                                  || step.kind == ContentStepKind::glimmerSite1Crew
                                  || step.kind == ContentStepKind::glimmerSite2Crew
                                  || step.kind == ContentStepKind::glimmerComplete
                                  || step.kind == ContentStepKind::glimmerNormalChest
                                  || step.kind == ContentStepKind::glimmerCleanup;
    }
    assert(!unsupportedStepDeclared);
    const ContentSignal* site0Spawned = nullptr;
    const ContentSignal* site0EnterPublished = nullptr;
    bool site1EnterPublished = false;
    bool site2EnterPublished = false;
    bool ambiguousShipEnteredSignal = false;
    for (std::size_t index = 0; index < trostlandResult.program.contentSignalCount; ++index) {
        const ContentSignal& signal = trostlandResult.program.contentSignals[index];
        const std::string_view name(signal.name.bytes.data(), signal.name.length);
        if (name == "site_1_ship_spawned") site0Spawned = &signal;
        if (name == "site_1_enter_published") site0EnterPublished = &signal;
        if (name == "site_2_enter_published") site1EnterPublished = true;
        if (name == "site_3_enter_published") site2EnterPublished = true;
        if (name == "site_1_ship_entered" || name == "site_2_ship_entered"
            || name == "site_3_ship_entered") ambiguousShipEnteredSignal = true;
    }
    assert(site0EnterPublished != nullptr
           && site0EnterPublished->id == 0x6FAD6FD8F15A56CAULL
           && site1EnterPublished && site2EnterPublished
           && !ambiguousShipEnteredSignal);
    const MissionTimer* site0EnterWatchdog = nullptr;
    for (std::size_t index = 0; index < trostlandResult.program.timerCount; ++index) {
        const MissionTimer& timer = trostlandResult.program.timers[index];
        if (std::string_view(timer.name.bytes.data(), timer.name.length)
            == "site_1_enter_watchdog") site0EnterWatchdog = &timer;
    }
    assert(introStep != nullptr && site0EnterStep != nullptr && site0Spawned != nullptr
           && site0EnterWatchdog != nullptr);

    const MissionTransition* site0Sequence = nullptr;
    const MissionTransition* site0EnterPublication = nullptr;
    for (std::size_t index = 0; index < trostlandResult.program.transitionCount; ++index) {
        const MissionTransition& transition = trostlandResult.program.transitions[index];
        if (transition.event == MissionEventKind::contentSignal
            && transition.source == site0Spawned->id && transition.requiredState == 1) {
            site0Sequence = &transition;
        }
        if (transition.event == MissionEventKind::contentSignal
            && transition.source == site0EnterPublished->id && transition.requiredState == 2) {
            site0EnterPublication = &transition;
        }
    }
    assert(site0EnterPublication != nullptr && site0EnterPublication->actionCount == 1
           && site0EnterPublication->actions[0].kind == MissionActionKind::changeMissionState
           && site0EnterPublication->actions[0].value == 3);
    assert(site0Sequence != nullptr && site0Sequence->actionCount == 4);
    assert(site0Sequence->actions[0].kind == MissionActionKind::activateContentStep
           && site0Sequence->actions[0].target == introStep->id);
    assert(site0Sequence->actions[1].kind == MissionActionKind::activateContentStep
           && site0Sequence->actions[1].target == site0EnterStep->id);
    assert(site0Sequence->actions[2].kind == MissionActionKind::scheduleTimer
           && site0Sequence->actions[2].target == site0EnterWatchdog->id);
    assert(site0Sequence->actions[3].kind == MissionActionKind::changeMissionState
           && site0Sequence->actions[3].value == 2);

    const MissionCompileResult first = compile_mission_source(kValid, "edz_freeroam");
    const MissionCompileResult second = compile_mission_source(kValid, "edz_freeroam");
    assert(first.status == MissionCompileStatus::success);
    assert(first.program.hash != 0 && first.program.hash == second.program.hash);
    assert(first.program.objectiveCount == 1 && first.program.interactionCount == 1);
    assert(first.program.waveCount == 1 && first.program.contentStepCount == 1
           && first.program.transitionCount == 1);
    assert(first.program.contentSteps[0].kind == ContentStepKind::glimmerIntro);
    const auto& interaction = first.program.interactions[0];
    assert(interaction.transform.position.x == 408.104538F);
    assert(interaction.transform.position.y == 406.008057F);
    assert(interaction.transform.position.z == 78.609955F);
    assert(interaction.transform.rotation.x == 0.0F);
    assert(interaction.transform.rotation.y == 0.0F);
    assert(interaction.transform.rotation.z == 0.0F);
    assert(interaction.transform.rotation.w == 1.0F);
    assert(interaction.extents.x == 75.0F);
    assert(interaction.extents.y == 60.0F);
    assert(interaction.extents.z == 10.0F);
    assert(first.program.waves[0].spawnerDefinition == 0x80C26B0A);
    assert(first.program.waves[0].requested[0] == 1 && first.program.waves[0].requested[1] == 0);

    const MissionCompileResult timed = compile_mission_source(kSignalsAndTimers, "edz_freeroam");
    const MissionCompileResult timedAgain = compile_mission_source(kSignalsAndTimers, "edz_freeroam");
    assert(timed.status == MissionCompileStatus::success);
    assert(timed.program.hash != 0 && timed.program.hash == timedAgain.program.hash);
    assert(timed.program.contentSignalCount == 1 && timed.program.timerCount == 1);
    assert(timed.program.contentSignals[0].name.length == 11);
    assert(timed.program.timers[0].delayTicks == 900);
    assert(timed.program.transitions[0].event == MissionEventKind::contentSignal);
    assert(timed.program.transitions[0].actions[0].kind == MissionActionKind::scheduleTimer);
    assert(timed.program.transitions[1].event == MissionEventKind::timerFired);

    assert(compile_mission_source(
               replace(kSignalsAndTimers, "source=\"site0_spawn\"", "source=\"missing\""),
               "edz_freeroam").status == MissionCompileStatus::validationError);
    assert(compile_mission_source(
               replace(kSignalsAndTimers, "target=\"site0_timeout\"", "target=\"missing\""),
               "edz_freeroam").status == MissionCompileStatus::validationError);
    assert(compile_mission_source(replace(kSignalsAndTimers, "delay=30", "delay=0"),
                                  "edz_freeroam").status == MissionCompileStatus::validationError);
    assert(compile_mission_source(
               replace(kSignalsAndTimers, "content_signals={{id=\"site0_spawn\"}}",
                       "content_signals={{id=\"site0_spawn\", slot_type=1}}"),
               "edz_freeroam").status == MissionCompileStatus::validationError);
    assert(compile_mission_source(
               replace(kSignalsAndTimers, "content_signals=1", "content_signals=17"),
               "edz_freeroam").status == MissionCompileStatus::validationError);
    assert(compile_mission_source(
               replace(kSignalsAndTimers, "content_signals={{id=\"site0_spawn\"}}",
                       "content_signals={{id=\"volume\"}}"),
               "edz_freeroam").status == MissionCompileStatus::validationError);

    assert(compile_mission_source("return mission {", "edz_freeroam").status
           == MissionCompileStatus::syntaxError);
    assert(compile_mission_source(kValid, "cosmodrome").status
           == MissionCompileStatus::validationError);
    assert(compile_mission_source(replace(kValid, "id=\"test\",", "id=\"test\", surprise=true,"),
                                  "edz_freeroam").status
           == MissionCompileStatus::validationError);
    assert(compile_mission_source(replace(kValid, "id=\"wave\"", "id=\"volume\""),
                                  "edz_freeroam").status
           == MissionCompileStatus::validationError);
    assert(compile_mission_source(replace(kValid, "target=\"wave\"", "target=\"missing\""),
                                  "edz_freeroam").status
           == MissionCompileStatus::validationError);
    assert(compile_mission_source(
               replace(kValid, "target=\"glimmer_intro\"", "target=\"missing_step\""),
               "edz_freeroam").status == MissionCompileStatus::validationError);
    assert(compile_mission_source(
               replace(kValid, "id=\"glimmer_intro\"", "id=\"unknown_step\""),
               "edz_freeroam").status == MissionCompileStatus::validationError);
    const auto unsupportedStepRejected = [](std::string_view name) {
        const std::string replacement = "id=\"" + std::string(name) + "\"";
        return compile_mission_source(
                   replace(kValid, "id=\"glimmer_intro\"", replacement),
                   "edz_freeroam").status == MissionCompileStatus::validationError;
    };
    assert(unsupportedStepRejected("glimmer_site_1_crew"));
    assert(unsupportedStepRejected("glimmer_site_2_crew"));
    assert(unsupportedStepRejected("glimmer_site_3_crew"));
    assert(unsupportedStepRejected("glimmer_complete"));
    assert(unsupportedStepRejected("glimmer_normal_chest"));
    assert(unsupportedStepRejected("glimmer_cleanup"));
    assert(compile_mission_source(replace(kValid, "x=75", "x=0/0"), "edz_freeroam").status
           == MissionCompileStatus::validationError);
    assert(compile_mission_source(replace(kValid, "triggers=1", "triggers=17"),
                                  "edz_freeroam").status
           == MissionCompileStatus::validationError);
    assert(compile_mission_source("while true do end", "edz_freeroam").status
           == MissionCompileStatus::validationError);
    assert(compile_mission_source(
               "if os ~= nil or io ~= nil or package ~= nil or debug ~= nil or require ~= nil then "
               "error('unsafe') end return " + std::string(kValid).substr(std::string(kValid).find("mission")),
               "edz_freeroam").status == MissionCompileStatus::success);
}
