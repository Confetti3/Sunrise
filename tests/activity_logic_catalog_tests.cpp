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
    constexpr std::size_t refs = entity + 2 * 48;
    constexpr std::size_t placement = refs + 2 * 4;
    constexpr std::size_t edges = placement + 44;
    constexpr std::size_t strings = edges + 16;
    const char text[] = "encounterdestspawn ruleStrong textobjective targetdestiny2-static-activity-logic-archive-v2";
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
    u32(bytes, 136, 0x32); u32(bytes, 140, 0x29);
    std::size_t d = 64;
    for (auto [off,count,stride] : std::array<std::array<std::uint32_t,3>,5>{{
        {activity,1,28},{entity,2,48},{refs,2,4},{placement,1,44},{edges,1,16}}}) {
        u32(bytes,d,off); u32(bytes,d+4,count); u32(bytes,d+8,stride); d += 12;
    }
    std::memcpy(bytes.data()+strings, text, stringBytes);
    // Activity: scenario 0x80ABCDEF, name "encounter", destination "dest", refs [entity0, entity1].
    u32(bytes,activity,0x80ABCDEF); u32(bytes,activity+4,0); u32(bytes,activity+8,9);
    u32(bytes,activity+12,9); u32(bytes,activity+16,4); u32(bytes,activity+20,0); u32(bytes,activity+24,2);
    // Entity 0: spawn_definition, name offset 13 len 10, label offset 13 len 10, localized 23 len 11.
    u32(bytes,entity,0x80B50027); u32(bytes,entity+4,0x808094CF); u32(bytes,entity+8,0x808094D0);
    bytes[entity+12]=static_cast<std::byte>(static_cast<std::uint8_t>(logic::Role::spawnDefinition));
    bytes[entity+13]=static_cast<std::byte>(static_cast<std::uint8_t>(logic::Confidence::strong));
    u32(bytes,entity+16,13); u32(bytes,entity+20,10);
    u32(bytes,entity+24,13); u32(bytes,entity+28,10);
    u32(bytes,entity+32,23); u32(bytes,entity+36,11);
    u32(bytes,entity+40,0); u32(bytes,entity+44,1);
    // Entity 1: objective, name offset 34 len 16, label offset 34 len 16.
    u32(bytes,entity+48,0x80B50028); u32(bytes,entity+52,0x808092AA); u32(bytes,entity+56,0x808092AB);
    bytes[entity+60]=static_cast<std::byte>(static_cast<std::uint8_t>(logic::Role::objective));
    bytes[entity+61]=static_cast<std::byte>(static_cast<std::uint8_t>(logic::Confidence::probable));
    u32(bytes,entity+64,34); u32(bytes,entity+68,16);
    u32(bytes,entity+72,34); u32(bytes,entity+76,16);
    u32(bytes,entity+80,0); u32(bytes,entity+84,0);
    u32(bytes,entity+88,0); u32(bytes,entity+92,0);
    u32(bytes,refs,0); u32(bytes,refs+4,1);
    u64(bytes,placement,0x143BDCF3C15846BBULL); u32(bytes,placement+8,0x80AA0001); u32(bytes,placement+12,0x80BB0001);
    f32(bytes,placement+16,-112.0F); f32(bytes,placement+20,136.0F); f32(bytes,placement+24,-8.0F);
    f32(bytes,placement+28,0); f32(bytes,placement+32,0); f32(bytes,placement+36,0); f32(bytes,placement+40,1);
    // Edge: entity0 -> entity1, nameHash 0x81112233, occurrence 4.
    u32(bytes,edges,0); u32(bytes,edges+4,1); u32(bytes,edges+8,0x81112233); u32(bytes,edges+12,4);
    return bytes;
}

int main() {
    auto bytes = fixture(); logic::Catalog catalog; std::string error;
    if (!require(logic::load(bytes,catalog,error), "valid activity logic catalog loads")) { std::printf("%s\n", error.c_str()); return 1; }
    const logic::Activity* activity = logic::find_activity(catalog,0x80ABCDEF);
    const logic::Entity* entity = logic::find_entity(catalog,0x80B50027);
    const logic::Entity* target = logic::find_entity(catalog,0x80B50028);
    if (!require(activity && activity->entityIndices.size()==2, "activity reference")
        || !require(entity && entity->placements.size()==1, "entity placement")
        || !require(target != nullptr, "second entity present")
        || !require(entity->placements[0].worldId==0x143BDCF3C15846BBULL, "world id retained")
        || !require(std::string(logic::role_name(entity->role))=="Squad spawn rule", "role label")
        || !require(std::string(logic::confidence_name(entity->confidence))=="strong", "confidence label")) return 2;
    auto outgoing = logic::outgoing_edges(catalog, 0);
    auto incoming = logic::incoming_edges(catalog, 1);
    if (!require(outgoing.size()==1, "one outgoing edge indexed")
        || !require(incoming.size()==1, "one incoming edge indexed")
        || !require(catalog.edges[outgoing[0]].sourceEntityIndex==0 && catalog.edges[outgoing[0]].targetEntityIndex==1, "edge direction preserved")
        || !require(catalog.edges[outgoing[0]].nameHash==0x81112233, "edge name hash preserved")
        || !require(catalog.edges[outgoing[0]].occurrenceCount==4, "edge occurrence count preserved")
        || !require(logic::outgoing_edges(catalog, 1).empty(), "target has no outgoing edges")
        || !require(logic::incoming_edges(catalog, 0).empty(), "source has no incoming edges")) return 3;
    auto bad = bytes; bad[0] = std::byte{0};
    logic::Catalog invalid; error.clear();
    if (!require(!logic::load(bad,invalid,error), "bad magic rejected")) return 4;
    std::printf("activity logic catalog tests passed\n");
    return 0;
}
