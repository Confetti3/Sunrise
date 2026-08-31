#include "mission_compiler.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace sunrise::server::gameplay::mission {
namespace {

constexpr std::size_t kLuaMemoryLimit = 1024 * 1024;
constexpr int kLuaInstructionLimit = 100'000;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct LuaMemory final {
    std::size_t used{};
    bool exhausted{};
};

void* lua_allocator(void* context, void* pointer, std::size_t oldSize, std::size_t newSize) {
    auto& memory = *static_cast<LuaMemory*>(context);
    if (pointer == nullptr) oldSize = 0;
    if (newSize == 0) {
        std::free(pointer);
        memory.used -= oldSize;
        return nullptr;
    }
    if (newSize > oldSize && newSize - oldSize > kLuaMemoryLimit - memory.used) {
        memory.exhausted = true;
        return nullptr;
    }
    void* result = std::realloc(pointer, newSize);
    if (result != nullptr) {
        memory.used = memory.used - oldSize + newSize;
    }
    return result;
}

void instruction_limit(lua_State* state, lua_Debug*) {
    luaL_error(state, "mission instruction limit exceeded");
}

int mission_identity(lua_State* state) {
    if (lua_gettop(state) != 1 || !lua_istable(state, 1)) {
        return luaL_error(state, "mission expects exactly one table");
    }
    lua_settop(state, 1);
    return 1;
}

void set_error(MissionCompileResult& result,
               MissionCompileStatus status,
               const char* format,
               const char* detail = nullptr) noexcept {
    result.status = status;
    if (detail == nullptr) {
        std::snprintf(result.error.data(), result.error.size(), "%s", format);
    } else {
        std::snprintf(result.error.data(), result.error.size(), format, detail);
    }
}

[[nodiscard]] bool allowed_key(const char* key,
                               const char* const* allowed,
                               std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (std::strcmp(key, allowed[index]) == 0) {
            return true;
        }
    }
    return false;
}

template <std::size_t Count>
[[nodiscard]] bool keys(lua_State* state,
                        int table,
                        const char* const (&allowed)[Count],
                        MissionCompileResult& result) noexcept {
    table = lua_absindex(state, table);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING
            || !allowed_key(lua_tostring(state, -2), allowed, Count)) {
            const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2)
                                                                  : "non-string key";
            set_error(result, MissionCompileStatus::validationError, "unknown field: %s", key);
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    return true;
}

[[nodiscard]] bool field_table(lua_State* state,
                               int table,
                               const char* name,
                               bool required,
                               MissionCompileResult& result) noexcept {
    lua_getfield(state, table, name);
    if (lua_istable(state, -1)) {
        return true;
    }
    if (!required && lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    set_error(result, MissionCompileStatus::validationError, "field is not a table: %s", name);
    lua_pop(state, 1);
    return false;
}

[[nodiscard]] bool text_value(lua_State* state,
                              int table,
                              const char* name,
                              MissionText& output,
                              MissionCompileResult& result) noexcept {
    lua_getfield(state, table, name);
    std::size_t length = 0;
    const char* value = lua_type(state, -1) == LUA_TSTRING
        ? lua_tolstring(state, -1, &length)
        : nullptr;
    if (value == nullptr || length == 0 || length >= output.bytes.size()) {
        set_error(result, MissionCompileStatus::validationError, "invalid text field: %s", name);
        lua_pop(state, 1);
        return false;
    }
    std::memcpy(output.bytes.data(), value, length);
    output.length = static_cast<std::uint8_t>(length);
    lua_pop(state, 1);
    return true;
}

template <typename Integer>
[[nodiscard]] bool integer_value(lua_State* state,
                                 int table,
                                 const char* name,
                                 Integer minimum,
                                 Integer maximum,
                                 Integer& output,
                                 MissionCompileResult& result) noexcept {
    lua_getfield(state, table, name);
    int exact = 0;
    const lua_Integer value = lua_tointegerx(state, -1, &exact);
    if (exact == 0 || value < static_cast<lua_Integer>(minimum)
        || static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(maximum)) {
        set_error(result, MissionCompileStatus::validationError, "invalid integer field: %s", name);
        lua_pop(state, 1);
        return false;
    }
    output = static_cast<Integer>(value);
    lua_pop(state, 1);
    return true;
}

template <typename Integer>
[[nodiscard]] bool optional_integer_value(lua_State* state,
                                          int table,
                                          const char* name,
                                          Integer minimum,
                                          Integer maximum,
                                          Integer& output,
                                          MissionCompileResult& result) noexcept {
    lua_getfield(state, table, name);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    int exact = 0;
    const lua_Integer value = lua_tointegerx(state, -1, &exact);
    if (exact == 0 || value < static_cast<lua_Integer>(minimum)
        || static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(maximum)) {
        set_error(result, MissionCompileStatus::validationError, "invalid integer field: %s", name);
        lua_pop(state, 1);
        return false;
    }
    output = static_cast<Integer>(value);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] bool number_value(lua_State* state,
                                int table,
                                const char* name,
                                float& output,
                                MissionCompileResult& result) noexcept {
    lua_getfield(state, table, name);
    int exact = 0;
    const lua_Number value = lua_tonumberx(state, -1, &exact);
    if (exact == 0 || !std::isfinite(value)
        || value < -static_cast<lua_Number>(std::numeric_limits<float>::max())
        || value > static_cast<lua_Number>(std::numeric_limits<float>::max())) {
        set_error(result, MissionCompileStatus::validationError, "invalid number field: %s", name);
        lua_pop(state, 1);
        return false;
    }
    output = static_cast<float>(value);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] std::uint64_t id_of(const MissionText& text) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t index = 0; index < text.length; ++index) {
        hash = (hash ^ static_cast<unsigned char>(text.bytes[index])) * kFnvPrime;
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] bool array_shape(lua_State* state,
                               int table,
                               std::size_t capacity,
                               std::size_t& length,
                               MissionCompileResult& result) noexcept {
    table = lua_absindex(state, table);
    length = lua_rawlen(state, table);
    if (length > capacity) {
        set_error(result, MissionCompileStatus::validationError, "capacity exceeded");
        return false;
    }
    std::size_t entries = 0;
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (!lua_isinteger(state, -2)) {
            set_error(result, MissionCompileStatus::validationError, "array has a non-integer key");
            lua_pop(state, 2);
            return false;
        }
        const lua_Integer key = lua_tointeger(state, -2);
        if (key < 1 || static_cast<std::size_t>(key) > length) {
            set_error(result, MissionCompileStatus::validationError, "array is sparse");
            lua_pop(state, 2);
            return false;
        }
        ++entries;
        lua_pop(state, 1);
    }
    if (entries != length) {
        set_error(result, MissionCompileStatus::validationError, "array is malformed");
        return false;
    }
    return true;
}

[[nodiscard]] bool vector(lua_State* state,
                          int table,
                          const char* name,
                          physics::world::Vector3& output,
                          bool positive,
                          MissionCompileResult& result) noexcept {
    if (!field_table(state, table, name, true, result)) {
        return false;
    }
    constexpr const char* fields[] = {"x", "y", "z"};
    const bool valid = keys(state, -1, fields, result) && number_value(state, -1, "x", output.x, result)
        && number_value(state, -1, "y", output.y, result)
        && number_value(state, -1, "z", output.z, result)
        && (!positive || (output.x > 0.0F && output.y > 0.0F && output.z > 0.0F));
    if (!valid && result.error[0] == '\0') {
        set_error(result, MissionCompileStatus::validationError, "extents must be positive");
    }
    lua_pop(state, 1);
    return valid;
}

[[nodiscard]] bool parse_budgets(lua_State* state,
                                 int table,
                                 MissionProgram& program,
                                 MissionCompileResult& result) noexcept {
    if (!field_table(state, table, "budgets", true, result)) {
        return false;
    }
    constexpr const char* fields[] = {"triggers", "objectives", "waves", "content_steps",
                                      "content_signals", "timers", "transitions"};
    const bool valid = keys(state, -1, fields, result)
        && integer_value(state, -1, "triggers", 0U, static_cast<std::uint32_t>(kInteractionCapacity), program.budgets.triggers, result)
        && integer_value(state, -1, "objectives", 0U, static_cast<std::uint32_t>(kObjectiveCapacity), program.budgets.objectives, result)
        && integer_value(state, -1, "waves", 0U, static_cast<std::uint32_t>(kEnemyWaveCapacity), program.budgets.waves, result)
        && integer_value(state, -1, "content_steps", 0U, static_cast<std::uint32_t>(kContentStepCapacity), program.budgets.contentSteps, result)
        && optional_integer_value(state, -1, "content_signals", 0U,
                                  static_cast<std::uint32_t>(kContentSignalCapacity),
                                  program.budgets.contentSignals, result)
        && optional_integer_value(state, -1, "timers", 0U,
                                  static_cast<std::uint32_t>(kTimerCapacity),
                                  program.budgets.timers, result)
        && integer_value(state, -1, "transitions", 0U, static_cast<std::uint32_t>(kTransitionCapacity), program.budgets.transitions, result);
    lua_pop(state, 1);
    return valid;
}

[[nodiscard]] bool duplicate_id(const MissionProgram& program,
                                std::uint64_t id,
                                std::size_t objectiveCount,
                                std::size_t interactionCount,
                                std::size_t waveCount,
                                std::size_t contentStepCount = 0,
                                std::size_t contentSignalCount = 0,
                                std::size_t timerCount = 0) noexcept {
    for (std::size_t index = 0; index < objectiveCount; ++index) {
        if (program.objectives[index].id == id) return true;
    }
    for (std::size_t index = 0; index < interactionCount; ++index) {
        if (program.interactions[index].id == id) return true;
    }
    for (std::size_t index = 0; index < waveCount; ++index) {
        if (program.waves[index].id == id) return true;
    }
    for (std::size_t index = 0; index < contentStepCount; ++index) {
        if (program.contentSteps[index].id == id) return true;
    }
    for (std::size_t index = 0; index < contentSignalCount; ++index) {
        if (program.contentSignals[index].id == id) return true;
    }
    for (std::size_t index = 0; index < timerCount; ++index) {
        if (program.timers[index].id == id) return true;
    }
    return false;
}

[[nodiscard]] ObjectiveState objective_state(const MissionText& text) noexcept {
    const std::string_view value{text.bytes.data(), text.length};
    if (value == "inactive") return ObjectiveState::inactive;
    if (value == "active") return ObjectiveState::active;
    if (value == "complete") return ObjectiveState::complete;
    return ObjectiveState::count;
}

[[nodiscard]] bool parse_objectives(lua_State* state,
                                    int root,
                                    MissionProgram& program,
                                    MissionCompileResult& result) noexcept {
    if (!field_table(state, root, "objectives", true, result)) return false;
    std::size_t count = 0;
    if (!array_shape(state, -1, kObjectiveCapacity, count, result)
        || count > program.budgets.objectives) {
        if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "objective budget exceeded");
        lua_pop(state, 1);
        return false;
    }
    const int array = lua_absindex(state, -1);
    constexpr const char* fields[] = {"id", "initial"};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, array, static_cast<lua_Integer>(index + 1));
        MissionText initial{};
        if (!lua_istable(state, -1) || !keys(state, -1, fields, result)
            || !text_value(state, -1, "id", program.objectives[index].name, result)
            || !text_value(state, -1, "initial", initial, result)) {
            lua_pop(state, 2);
            return false;
        }
        program.objectives[index].id = id_of(program.objectives[index].name);
        program.objectives[index].initialState = objective_state(initial);
        if (program.objectives[index].initialState == ObjectiveState::count
            || duplicate_id(program, program.objectives[index].id, index, 0, 0)) {
            set_error(result, MissionCompileStatus::validationError, "invalid or duplicate objective");
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    program.objectiveCount = static_cast<std::uint8_t>(count);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] ContentStepKind content_step_kind(const MissionText& text) noexcept {
    const std::string_view value{text.bytes.data(), text.length};
    if (value == "glimmer_intro") return ContentStepKind::glimmerIntro;
    if (value == "glimmer_site_1_ship_spawn") return ContentStepKind::glimmerSite0ShipSpawn;
    if (value == "glimmer_site_1_enter") return ContentStepKind::glimmerSite0Enter;
    if (value == "glimmer_site_1_exit") return ContentStepKind::glimmerSite0Exit;
    if (value == "glimmer_site_2_ship_spawn") return ContentStepKind::glimmerSite1ShipSpawn;
    if (value == "glimmer_site_2_enter") return ContentStepKind::glimmerSite1Enter;
    if (value == "glimmer_site_2_exit") return ContentStepKind::glimmerSite1Exit;
    if (value == "glimmer_site_3_ship_spawn") return ContentStepKind::glimmerSite2ShipSpawn;
    if (value == "glimmer_site_3_enter") return ContentStepKind::glimmerSite2Enter;
    if (value == "glimmer_site_3_exit") return ContentStepKind::glimmerSite2Exit;
    // Crew, completion, chest, and cleanup remain compiler-invisible until an exact package body
    // and an authoritative lifecycle producer are both independently established.
    return ContentStepKind::count;
}

[[nodiscard]] bool parse_content_steps(lua_State* state,
                                       int root,
                                       MissionProgram& program,
                                       MissionCompileResult& result) noexcept {
    if (!field_table(state, root, "content_steps", true, result)) return false;
    std::size_t count = 0;
    if (!array_shape(state, -1, kContentStepCapacity, count, result)
        || count > program.budgets.contentSteps) {
        if (result.error[0] == '\0') {
            set_error(result, MissionCompileStatus::validationError,
                      "content-step budget exceeded");
        }
        lua_pop(state, 1);
        return false;
    }
    const int array = lua_absindex(state, -1);
    constexpr const char* fields[] = {"id"};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, array, static_cast<lua_Integer>(index + 1));
        auto& item = program.contentSteps[index];
        if (!lua_istable(state, -1) || !keys(state, -1, fields, result)
            || !text_value(state, -1, "id", item.name, result)) {
            lua_pop(state, 2);
            return false;
        }
        item.id = id_of(item.name);
        item.kind = content_step_kind(item.name);
        if (item.kind == ContentStepKind::count
            || duplicate_id(program, item.id, program.objectiveCount,
                            program.interactionCount, program.waveCount, index)) {
            set_error(result, MissionCompileStatus::validationError,
                      "invalid or duplicate content step");
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    program.contentStepCount = static_cast<std::uint8_t>(count);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] bool parse_content_signals(lua_State* state,
                                         int root,
                                         MissionProgram& program,
                                         MissionCompileResult& result) noexcept {
    if (!field_table(state, root, "content_signals", false, result)) {
        return result.error[0] == '\0';
    }
    std::size_t count = 0;
    if (!array_shape(state, -1, kContentSignalCapacity, count, result)
        || count > program.budgets.contentSignals) {
        if (result.error[0] == '\0') {
            set_error(result, MissionCompileStatus::validationError,
                      "content-signal budget exceeded");
        }
        lua_pop(state, 1);
        return false;
    }
    const int array = lua_absindex(state, -1);
    constexpr const char* fields[] = {"id"};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, array, static_cast<lua_Integer>(index + 1));
        auto& item = program.contentSignals[index];
        if (!lua_istable(state, -1) || !keys(state, -1, fields, result)
            || !text_value(state, -1, "id", item.name, result)) {
            lua_pop(state, 2);
            return false;
        }
        item.id = id_of(item.name);
        if (duplicate_id(program, item.id, program.objectiveCount, program.interactionCount,
                         program.waveCount, program.contentStepCount, index)) {
            set_error(result, MissionCompileStatus::validationError,
                      "invalid or duplicate content signal");
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    program.contentSignalCount = static_cast<std::uint8_t>(count);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] bool parse_timers(lua_State* state,
                                int root,
                                MissionProgram& program,
                                MissionCompileResult& result) noexcept {
    if (!field_table(state, root, "timers", false, result)) {
        return result.error[0] == '\0';
    }
    std::size_t count = 0;
    if (!array_shape(state, -1, kTimerCapacity, count, result) || count > program.budgets.timers) {
        if (result.error[0] == '\0') {
            set_error(result, MissionCompileStatus::validationError, "timer budget exceeded");
        }
        lua_pop(state, 1);
        return false;
    }
    const int array = lua_absindex(state, -1);
    constexpr const char* fields[] = {"id", "delay"};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, array, static_cast<lua_Integer>(index + 1));
        auto& item = program.timers[index];
        std::uint32_t delaySeconds = 0;
        if (!lua_istable(state, -1) || !keys(state, -1, fields, result)
            || !text_value(state, -1, "id", item.name, result)
            || !integer_value(state, -1, "delay", 1U,
                              (std::numeric_limits<std::uint32_t>::max)()
                                  / physics::world::kDefaultFixedRateHz,
                              delaySeconds, result)) {
            lua_pop(state, 2);
            return false;
        }
        item.delayTicks = delaySeconds * physics::world::kDefaultFixedRateHz;
        item.id = id_of(item.name);
        if (duplicate_id(program, item.id, program.objectiveCount, program.interactionCount,
                         program.waveCount, program.contentStepCount,
                         program.contentSignalCount, index)) {
            set_error(result, MissionCompileStatus::validationError,
                      "invalid or duplicate timer");
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    program.timerCount = static_cast<std::uint8_t>(count);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] bool parse_interactions(lua_State* state,
                                      int root,
                                      MissionProgram& program,
                                      MissionCompileResult& result) noexcept {
    if (!field_table(state, root, "interactions", true, result)) return false;
    std::size_t count = 0;
    if (!array_shape(state, -1, kInteractionCapacity, count, result)
        || count > program.budgets.triggers) {
        if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "trigger budget exceeded");
        lua_pop(state, 1);
        return false;
    }
    const int array = lua_absindex(state, -1);
    constexpr const char* fields[] = {"id", "bubble", "position", "extents"};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, array, static_cast<lua_Integer>(index + 1));
        auto& item = program.interactions[index];
        if (!lua_istable(state, -1) || !keys(state, -1, fields, result)
            || !text_value(state, -1, "id", item.name, result)
            || !integer_value(state, -1, "bubble", 1U, std::numeric_limits<std::uint32_t>::max(), item.bubble, result)
            || !vector(state, -1, "position", item.transform.position, false, result)
            || !vector(state, -1, "extents", item.extents, true, result)) {
            lua_pop(state, 2);
            return false;
        }
        item.id = id_of(item.name);
        if (duplicate_id(program, item.id, program.objectiveCount, index, 0)) {
            set_error(result, MissionCompileStatus::validationError, "duplicate interaction id");
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    program.interactionCount = static_cast<std::uint8_t>(count);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] bool parse_waves(lua_State* state,
                               int root,
                               MissionProgram& program,
                               MissionCompileResult& result) noexcept {
    if (!field_table(state, root, "waves", true, result)) return false;
    std::size_t count = 0;
    if (!array_shape(state, -1, kEnemyWaveCapacity, count, result) || count > program.budgets.waves) {
        if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "wave budget exceeded");
        lua_pop(state, 1);
        return false;
    }
    const int array = lua_absindex(state, -1);
    constexpr const char* fields[] = {"id", "spawner", "mode", "requested"};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, array, static_cast<lua_Integer>(index + 1));
        auto& item = program.waves[index];
        if (!lua_istable(state, -1) || !keys(state, -1, fields, result)
            || !text_value(state, -1, "id", item.name, result)
            || !integer_value(state, -1, "spawner", 1U, std::numeric_limits<std::uint32_t>::max(), item.spawnerDefinition, result)
            || !integer_value(state, -1, "mode", static_cast<std::uint8_t>(0), static_cast<std::uint8_t>(4), item.mode, result)
            || !field_table(state, -1, "requested", true, result)) {
            lua_pop(state, 2);
            return false;
        }
        std::size_t requested = 0;
        if (!array_shape(state, -1, kRequestedSlotCapacity, requested, result) || requested == 0) {
            if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "requested slots are empty");
            lua_pop(state, 3);
            return false;
        }
        for (std::size_t slot = 0; slot < requested; ++slot) {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(slot + 1));
            int exact = 0;
            const lua_Integer value = lua_tointegerx(state, -1, &exact);
            lua_pop(state, 1);
            if (exact == 0 || value < 0 || value > 255) {
                set_error(result, MissionCompileStatus::validationError, "invalid requested member count");
                lua_pop(state, 3);
                return false;
            }
            item.requested[slot] = static_cast<std::uint32_t>(value);
        }
        item.requestedCount = static_cast<std::uint8_t>(requested);
        lua_pop(state, 1);
        item.id = id_of(item.name);
        if (duplicate_id(program, item.id, program.objectiveCount, program.interactionCount, index)) {
            set_error(result, MissionCompileStatus::validationError, "duplicate wave id");
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    program.waveCount = static_cast<std::uint8_t>(count);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] MissionEventKind event_kind(const MissionText& text) noexcept {
    const std::string_view value{text.bytes.data(), text.length};
    if (value == "initialized") return MissionEventKind::initialized;
    if (value == "trigger_enter") return MissionEventKind::triggerEnter;
    if (value == "trigger_stay") return MissionEventKind::triggerStay;
    if (value == "trigger_leave") return MissionEventKind::triggerLeave;
    if (value == "content_signal") return MissionEventKind::contentSignal;
    if (value == "timer_fired") return MissionEventKind::timerFired;
    return MissionEventKind::count;
}

[[nodiscard]] MissionActionKind action_kind(const MissionText& text) noexcept {
    const std::string_view value{text.bytes.data(), text.length};
    if (value == "activate_wave") return MissionActionKind::activateWave;
    if (value == "activate_content_step") return MissionActionKind::activateContentStep;
    if (value == "schedule_timer") return MissionActionKind::scheduleTimer;
    if (value == "change_objective") return MissionActionKind::changeObjective;
    if (value == "emit_incident") return MissionActionKind::emitIncident;
    if (value == "change_mission_state") return MissionActionKind::changeMissionState;
    return MissionActionKind::count;
}

[[nodiscard]] std::uint64_t named_id(lua_State* state,
                                     int table,
                                     const char* name,
                                     MissionCompileResult& result) noexcept {
    MissionText text{};
    return text_value(state, table, name, text, result) ? id_of(text) : 0;
}

[[nodiscard]] bool parse_transitions(lua_State* state,
                                     int root,
                                     MissionProgram& program,
                                     MissionCompileResult& result) noexcept {
    if (!field_table(state, root, "transitions", true, result)) return false;
    std::size_t count = 0;
    if (!array_shape(state, -1, kTransitionCapacity, count, result)
        || count > program.budgets.transitions) {
        if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "transition budget exceeded");
        lua_pop(state, 1);
        return false;
    }
    const int array = lua_absindex(state, -1);
    constexpr const char* fields[] = {"event", "source", "from_state", "actions"};
    constexpr const char* actionFields[] = {"type", "target", "value"};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, array, static_cast<lua_Integer>(index + 1));
        MissionText event{};
        if (!lua_istable(state, -1) || !keys(state, -1, fields, result)
            || !text_value(state, -1, "event", event, result)) {
            lua_pop(state, 2);
            return false;
        }
        auto& transition = program.transitions[index];
        transition.event = event_kind(event);
        transition.source = named_id(state, -1, "source", result);
        if (!optional_integer_value(state,
                                    -1,
                                    "from_state",
                                    0U,
                                    (std::numeric_limits<std::uint32_t>::max)() - 1U,
                                    transition.requiredState,
                                    result)) {
            lua_pop(state, 2);
            return false;
        }
        if (transition.event == MissionEventKind::count || transition.source == 0
            || !field_table(state, -1, "actions", true, result)) {
            if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "invalid transition");
            lua_pop(state, 2);
            return false;
        }
        std::size_t actions = 0;
        if (!array_shape(state, -1, kActionsPerTransition, actions, result) || actions == 0) {
            if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "transition has no actions");
            lua_pop(state, 3);
            return false;
        }
        const int actionArray = lua_absindex(state, -1);
        for (std::size_t actionIndex = 0; actionIndex < actions; ++actionIndex) {
            lua_rawgeti(state, actionArray, static_cast<lua_Integer>(actionIndex + 1));
            MissionText type{};
            auto& action = transition.actions[actionIndex];
            if (!lua_istable(state, -1) || !keys(state, -1, actionFields, result)
                || !text_value(state, -1, "type", type, result)) {
                lua_pop(state, 4);
                return false;
            }
            action.kind = action_kind(type);
            if (action.kind == MissionActionKind::activateWave
                || action.kind == MissionActionKind::activateContentStep
                || action.kind == MissionActionKind::scheduleTimer
                || action.kind == MissionActionKind::changeObjective) {
                action.target = named_id(state, -1, "target", result);
            }
            if (action.kind == MissionActionKind::changeObjective
                || action.kind == MissionActionKind::emitIncident
                || action.kind == MissionActionKind::changeMissionState) {
                if (!integer_value(state, -1, "value", 0ULL, std::numeric_limits<std::uint64_t>::max(), action.value, result)) {
                    lua_pop(state, 4);
                    return false;
                }
            }
            if (action.kind == MissionActionKind::count
                || ((action.kind == MissionActionKind::activateWave
                    || action.kind == MissionActionKind::activateContentStep
                    || action.kind == MissionActionKind::scheduleTimer
                    || action.kind == MissionActionKind::changeObjective)
                    && action.target == 0)) {
                set_error(result, MissionCompileStatus::validationError, "invalid action");
                lua_pop(state, 4);
                return false;
            }
            lua_pop(state, 1);
        }
        transition.actionCount = static_cast<std::uint8_t>(actions);
        lua_pop(state, 2);
    }
    program.transitionCount = static_cast<std::uint8_t>(count);
    lua_pop(state, 1);
    return true;
}

[[nodiscard]] bool has_interaction(const MissionProgram& program, std::uint64_t id) noexcept {
    for (std::size_t i = 0; i < program.interactionCount; ++i) if (program.interactions[i].id == id) return true;
    return false;
}
[[nodiscard]] bool has_wave(const MissionProgram& program, std::uint64_t id) noexcept {
    for (std::size_t i = 0; i < program.waveCount; ++i) if (program.waves[i].id == id) return true;
    return false;
}
[[nodiscard]] bool has_content_step(const MissionProgram& program, std::uint64_t id) noexcept {
    for (std::size_t i = 0; i < program.contentStepCount; ++i) {
        if (program.contentSteps[i].id == id) return true;
    }
    return false;
}
[[nodiscard]] bool has_content_signal(const MissionProgram& program, std::uint64_t id) noexcept {
    for (std::size_t i = 0; i < program.contentSignalCount; ++i) {
        if (program.contentSignals[i].id == id) return true;
    }
    return false;
}
[[nodiscard]] bool has_timer(const MissionProgram& program, std::uint64_t id) noexcept {
    for (std::size_t i = 0; i < program.timerCount; ++i) {
        if (program.timers[i].id == id) return true;
    }
    return false;
}
[[nodiscard]] bool has_objective(const MissionProgram& program, std::uint64_t id) noexcept {
    for (std::size_t i = 0; i < program.objectiveCount; ++i) if (program.objectives[i].id == id) return true;
    return false;
}

[[nodiscard]] bool references_valid(const MissionProgram& program) noexcept {
    for (std::size_t i = 0; i < program.transitionCount; ++i) {
        const auto& transition = program.transitions[i];
        if (transition.event != MissionEventKind::initialized
            && transition.event != MissionEventKind::contentSignal
            && transition.event != MissionEventKind::timerFired
            && !has_interaction(program, transition.source)) return false;
        if (transition.event == MissionEventKind::contentSignal
            && !has_content_signal(program, transition.source)) return false;
        if (transition.event == MissionEventKind::timerFired
            && !has_timer(program, transition.source)) return false;
        for (std::size_t j = 0; j < transition.actionCount; ++j) {
            const auto& action = transition.actions[j];
            if (action.kind == MissionActionKind::activateWave && !has_wave(program, action.target)) return false;
            if (action.kind == MissionActionKind::activateContentStep
                && !has_content_step(program, action.target)) return false;
            if (action.kind == MissionActionKind::scheduleTimer
                && !has_timer(program, action.target)) return false;
            if (action.kind == MissionActionKind::changeObjective && !has_objective(program, action.target)) return false;
            if (action.kind == MissionActionKind::changeObjective && action.value >= static_cast<std::uint64_t>(ObjectiveState::count)) return false;
        }
    }
    return true;
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t length) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < length; ++index) hash = (hash ^ bytes[index]) * kFnvPrime;
}
template <typename Value> void hash_value(std::uint64_t& hash, const Value& value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto byte = static_cast<unsigned char>((static_cast<std::uint64_t>(value) >> (index * 8U)) & 0xFFU);
        hash = (hash ^ byte) * kFnvPrime;
    }
}
void hash_float(std::uint64_t& hash, float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_value(hash, bits);
}

[[nodiscard]] std::uint64_t program_hash(const MissionProgram& p) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_bytes(hash, p.identity.bytes.data(), p.identity.length);
    hash_bytes(hash, p.destination.bytes.data(), p.destination.length);
    hash_value(hash, p.version);
    hash_value(hash, p.budgets.triggers); hash_value(hash, p.budgets.objectives);
    hash_value(hash, p.budgets.waves); hash_value(hash, p.budgets.contentSteps);
    hash_value(hash, p.budgets.contentSignals); hash_value(hash, p.budgets.timers);
    hash_value(hash, p.budgets.transitions);
    for (std::size_t i = 0; i < p.objectiveCount; ++i) { hash_value(hash, p.objectives[i].id); hash_value(hash, p.objectives[i].initialState); }
    for (std::size_t i = 0; i < p.interactionCount; ++i) {
        const auto& v = p.interactions[i]; hash_value(hash, v.id); hash_value(hash, v.bubble);
        hash_float(hash, v.transform.position.x); hash_float(hash, v.transform.position.y); hash_float(hash, v.transform.position.z);
        hash_float(hash, v.transform.rotation.x); hash_float(hash, v.transform.rotation.y); hash_float(hash, v.transform.rotation.z); hash_float(hash, v.transform.rotation.w);
        hash_float(hash, v.extents.x); hash_float(hash, v.extents.y); hash_float(hash, v.extents.z);
    }
    for (std::size_t i = 0; i < p.waveCount; ++i) {
        const auto& v = p.waves[i]; hash_value(hash, v.id); hash_value(hash, v.spawnerDefinition); hash_value(hash, v.mode); hash_value(hash, v.requestedCount);
        for (std::size_t j = 0; j < v.requestedCount; ++j) hash_value(hash, v.requested[j]);
    }
    for (std::size_t i = 0; i < p.contentStepCount; ++i) {
        hash_value(hash, p.contentSteps[i].id);
        hash_value(hash, p.contentSteps[i].kind);
    }
    for (std::size_t i = 0; i < p.contentSignalCount; ++i) {
        hash_value(hash, p.contentSignals[i].id);
    }
    for (std::size_t i = 0; i < p.timerCount; ++i) {
        hash_value(hash, p.timers[i].id);
        hash_value(hash, p.timers[i].delayTicks);
    }
    for (std::size_t i = 0; i < p.transitionCount; ++i) {
        const auto& v = p.transitions[i]; hash_value(hash, v.event); hash_value(hash, v.source); hash_value(hash, v.requiredState); hash_value(hash, v.actionCount);
        for (std::size_t j = 0; j < v.actionCount; ++j) { hash_value(hash, v.actions[j].kind); hash_value(hash, v.actions[j].target); hash_value(hash, v.actions[j].value); }
    }
    return hash;
}

[[nodiscard]] bool parse_program(lua_State* state,
                                 std::string_view expectedDestination,
                                 MissionCompileResult& result) noexcept {
    constexpr const char* fields[] = {"id", "version", "destination", "budgets",
                                      "objectives", "interactions", "waves",
                                      "content_steps", "content_signals", "timers",
                                      "transitions"};
    MissionProgram& program = result.program;
    if (!keys(state, -1, fields, result)
        || !text_value(state, -1, "id", program.identity, result)
        || !integer_value(state, -1, "version", 1U, std::numeric_limits<std::uint32_t>::max(), program.version, result)
        || !text_value(state, -1, "destination", program.destination, result)
        || std::string_view(program.destination.bytes.data(), program.destination.length) != expectedDestination
        || !parse_budgets(state, -1, program, result)
        || !parse_objectives(state, -1, program, result)
        || !parse_interactions(state, -1, program, result)
        || !parse_waves(state, -1, program, result)
        || !parse_content_steps(state, -1, program, result)
        || !parse_content_signals(state, -1, program, result)
        || !parse_timers(state, -1, program, result)
        || !parse_transitions(state, -1, program, result)) {
        if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "destination mismatch");
        return false;
    }
    if (!references_valid(program)) {
        set_error(result, MissionCompileStatus::validationError, "invalid mission reference");
        return false;
    }
    program.hash = program_hash(program);
    return true;
}

} // namespace

MissionCompileResult compile_mission_source(std::string_view source,
                                            std::string_view destination) noexcept {
    MissionCompileResult result{};
    if (source.size() > kMissionFileByteLimit) {
        set_error(result, MissionCompileStatus::tooLarge, "mission file is too large");
        return result;
    }
    if (destination.empty() || destination.size() >= kMissionTextCapacity) {
        set_error(result, MissionCompileStatus::validationError, "invalid destination");
        return result;
    }
    LuaMemory memory{};
    lua_State* state = lua_newstate(lua_allocator, &memory);
    if (state == nullptr) {
        set_error(result, MissionCompileStatus::resourceLimit, "unable to create Lua compiler");
        return result;
    }
    lua_pushcfunction(state, mission_identity);
    lua_setglobal(state, "mission");
    lua_sethook(state, instruction_limit, LUA_MASKCOUNT, kLuaInstructionLimit);
    const int load = luaL_loadbufferx(state, source.data(), source.size(), "mission", "t");
    if (load != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        set_error(result, memory.exhausted ? MissionCompileStatus::resourceLimit
                                          : MissionCompileStatus::syntaxError,
                  "%s",
                  error == nullptr ? "Lua syntax error" : error);
        lua_close(state);
        return result;
    }
    const int call = lua_pcall(state, 0, 1, 0);
    if (call != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        set_error(result, memory.exhausted ? MissionCompileStatus::resourceLimit
                                          : MissionCompileStatus::validationError,
                  "%s",
                  error == nullptr ? "Lua execution failed" : error);
        lua_close(state);
        return result;
    }
    if (!lua_istable(state, -1) || !parse_program(state, destination, result)) {
        if (result.error[0] == '\0') set_error(result, MissionCompileStatus::validationError, "mission must return one table");
        lua_close(state);
        return result;
    }
    lua_close(state);
    result.status = MissionCompileStatus::success;
    result.error = {};
    return result;
}

MissionCompileResult compile_mission_for_destination(const std::filesystem::path& missionDirectory,
                                                     std::string_view destination) noexcept {
    MissionCompileResult result{};
    if (destination.empty() || destination.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string_view::npos) {
        set_error(result, MissionCompileStatus::validationError, "invalid destination path");
        return result;
    }
    const std::filesystem::path path = missionDirectory / (std::string(destination) + ".lua");
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        set_error(result, error ? MissionCompileStatus::ioError : MissionCompileStatus::missing,
                  error ? "mission file lookup failed" : "mission file is missing");
        return result;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        set_error(result, MissionCompileStatus::ioError, "mission file size failed");
        return result;
    }
    if (size > kMissionFileByteLimit) {
        set_error(result, MissionCompileStatus::tooLarge, "mission file is too large");
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        set_error(result, MissionCompileStatus::ioError, "mission file open failed");
        return result;
    }
    std::string source(static_cast<std::size_t>(size), '\0');
    if (!source.empty() && !stream.read(source.data(), static_cast<std::streamsize>(source.size()))) {
        set_error(result, MissionCompileStatus::ioError, "mission file read failed");
        return result;
    }
    return compile_mission_source(source, destination);
}

} // namespace sunrise::server::gameplay::mission
