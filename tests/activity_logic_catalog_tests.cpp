#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "client/inspection/activity_logic_catalog.h"

namespace logic = sunrise::client::inspection::activity_logic_catalog;

[[nodiscard]] bool require(bool condition, const char* message) {
    if (!condition) std::printf("FAIL: %s\n", message);
    return condition;
}

void u32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
}
void u64(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[at + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
}
void f32(std::vector<std::byte>& bytes, std::size_t at, float value) {
    std::uint32_t bits{}; std::memcpy(&bits, &value, sizeof(bits)); u32(bytes, at, bits);
}

std::vector<std::byte> fixture() {
    constexpr std::size_t activity = logic::kHeaderSize;
    constexpr std::size_t entity = activity + 28;
    constexpr std::size_t refs = entity + 48;
    constexpr std::size_t placement = refs + 4;
    constexpr std::size_t edges = placement + 44;
    constexpr std::size_t strings = edges;
    const char text[] = "encounterdestspawn ruleStrong textdestiny2-static-activity-logic-archive-v2";
    constexpr std::size_t stringBytes = sizeof(text) - 1;
    std::vector<std::byte> bytes(strings + stringBytes);
    const char magic[] = "SLOGIC01";
    std::memcpy(bytes.data(), magic, 8);
    u32(bytes, 8, logic::kSchemaVersion); u32(bytes, 12, logic::kHeaderSize);
    u32(bytes, 16, static_cast<std::uint32_t>(bytes.size()));
    u32(bytes, 20, static_cast<std::uint32_t>(strings)); u32(bytes, 24, stringBytes);
    u32(bytes, 60, logic::kConverterVersion);
    // Provenance: contentBuild (0), generationTimestamp, sourceFormat offset/length.
    u32(bytes, 124, 0); u64(bytes, 128, 0);
    u32(bytes, 136, 0x22); u32(bytes, 140, 0x29);
    std::size_t d = 64;
    for (auto [off,count,stride] : std::array<std::array<std::uint32_t,3>,5>{{
        {activity,1,28},{entity,1,48},{refs,1,4},{placement,1,44},{edges,0,16}}}) {
        u32(bytes,d,off); u32(bytes,d+4,count); u32(bytes,d+8,stride); d += 12;
    }
    std::memcpy(bytes.data()+strings, text, stringBytes);
    u32(bytes,activity,0x80ABCDEF); u32(bytes,activity+4,0); u32(bytes,activity+8,9);
    u32(bytes,activity+12,9); u32(bytes,activity+16,4); u32(bytes,activity+20,0); u32(bytes,activity+24,1);
    u32(bytes,entity,0x80B50027); u32(bytes,entity+4,0x808094CF); u32(bytes,entity+8,0x808094D0);
    bytes[entity+12]=static_cast<std::byte>(static_cast<std::uint8_t>(logic::Role::spawnDefinition));
    bytes[entity+13]=static_cast<std::byte>(static_cast<std::uint8_t>(logic::Confidence::strong));
    u32(bytes,entity+16,13); u32(bytes,entity+20,10);
    u32(bytes,entity+24,13); u32(bytes,entity+28,10);
    u32(bytes,entity+32,23); u32(bytes,entity+36,11);
    u32(bytes,entity+40,0); u32(bytes,entity+44,1);
    u32(bytes,refs,0);
    u64(bytes,placement,0x143BDCF3C15846BBULL); u32(bytes,placement+8,0x80AA0001); u32(bytes,placement+12,0x80BB0001);
    f32(bytes,placement+16,-112.0F); f32(bytes,placement+20,136.0F); f32(bytes,placement+24,-8.0F);
    f32(bytes,placement+28,0); f32(bytes,placement+32,0); f32(bytes,placement+36,0); f32(bytes,placement+40,1);
    return bytes;
}

int main() {
    auto bytes = fixture(); logic::Catalog catalog; std::string error;
    if (!require(logic::load(bytes,catalog,error), "valid activity logic catalog loads")) { std::printf("%s\n", error.c_str()); return 1; }
    const logic::Activity* activity = logic::find_activity(catalog,0x80ABCDEF);
    const logic::Entity* entity = logic::find_entity(catalog,0x80B50027);
    if (!require(activity && activity->entityIndices.size()==1, "activity reference")
        || !require(entity && entity->placements.size()==1, "entity placement")
        || !require(entity->placements[0].worldId==0x143BDCF3C15846BBULL, "world id retained")
        || !require(std::string(logic::role_name(entity->role))=="Squad spawn rule", "role label")
        || !require(std::string(logic::confidence_name(entity->confidence))=="strong", "confidence label")) return 2;
    auto bad = bytes; bad[0] = std::byte{0};
    logic::Catalog invalid; error.clear();
    if (!require(!logic::load(bad,invalid,error), "bad magic rejected")) return 3;
    auto nonfinite = bytes; f32(nonfinite, logic::kHeaderSize + 28 + 4*0 /* no-op anchor */, 0.0F);
    std::printf("activity logic catalog tests passed\n");
    return 0;
}
