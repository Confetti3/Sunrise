#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "client/content/activity/activity_statevars.h"

namespace parser = sunrise::client::content::activity::statevars;
namespace logic = sunrise::client::inspection::activity_logic_catalog;

namespace {

template <typename T>
void put(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

void array(std::vector<std::byte>& bytes,
           std::size_t descriptor,
           std::size_t header,
           std::size_t count,
           std::uint32_t elementClass) {
    put(bytes, descriptor, static_cast<std::uint64_t>(count));
    put(bytes, descriptor + 8U, static_cast<std::int64_t>(header) - static_cast<std::int64_t>(descriptor + 8U));
    put(bytes, header - 4U, 0x80809FBDU);
    put(bytes, header, static_cast<std::uint64_t>(count));
    put(bytes, header + 8U, elementClass);
}

bool check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
    }
    return value;
}

} // namespace

int main() {
    bool valid = true;
    std::vector<std::byte> owner(512);
    array(owner, 0x10, 128, 1, 0x80809C04U);
    put(owner, 144, 0x80810001U);
    array(owner, 0x68, 256, 2, 0x80809C20U);
    put(owner, 272, 0x80810001U);
    put(owner, 276, 0x80804DE4U);
    put(owner, 280, 128U);
    put(owner, 296, 0x80810001U);
    put(owner, 300, 0x80804DE4U);
    put(owner, 304, 128U);
    std::vector<parser::OwnerRow> rows;
    std::vector<std::uint32_t> canonicalConfigs;
    std::string error;
    valid = check(parser::parse_owner_rows(
                      owner, 0x80809C0FU, rows, canonicalConfigs, error)
                      && rows.size() == 1 && canonicalConfigs.size() == 1
                      && rows.front().configTag == 0x80810001U,
                  "valid owner rows were not parsed and deduplicated")
            && valid;
    put(owner, 280, 64U);
    valid = check(!parser::parse_owner_rows(
                      owner, 0x80809C0FU, rows, canonicalConfigs, error),
                  "wrong StateVar component size was accepted")
            && valid;
    put(owner, 280, 128U);
    put(owner, 272, 0x80810002U);
    valid = check(!parser::parse_owner_rows(
                      owner, 0x80809C0FU, rows, canonicalConfigs, error),
                  "StateVar component missing canonical config was accepted")
            && valid;
    valid = check(!parser::parse_owner_rows(
                      owner, 0x80809C0EU, rows, canonicalConfigs, error),
                  "wrong StateVar owner class was accepted")
            && valid;

    std::vector<std::byte> config(1024);
    put(config, 8, static_cast<std::int64_t>(120));
    put(config, 124, 0x80804DE4U);
    put(config, 16, static_cast<std::int64_t>(240));
    put(config, 252, 0x80804DE8U);
    put(config, 352, 0x82E6ECA4U);
    put(config, 356, 7);
    put(config, 360, -1);
    put(config, 364, 1);
    put(config, 368, -1);
    put(config, 372, -1);
    put(config, 392, 1U);
    array(config, 376, 600, 1, 0x80804DE7U);
    put(config, 616, 10);
    put(config, 620, 20);
    put(config, 624, 0U);
    put(config, 632, 0x80820001U);
    array(config, 408, 700, 1, 0x80800009U);
    array(config, 424, 800, 1, 0x80800090U);
    logic::StateVar stateVar{};
    valid = check(parser::parse_config(config, 0x80809C36U, 0x80810001U, stateVar, error)
                      && stateVar.nameHash == 0x82E6ECA4U && stateVar.initial == 7
                      && stateVar.projectionEnabled && stateVar.projectionBytecodeCount == 1
                      && stateVar.projectionConstantCount == 1 && stateVar.triggers.size() == 1
                      && stateVar.triggers.front().referenceTag == 0,
                  "valid StateVar config was not parsed")
            && valid;
    put(config, 708, 0x80800008U);
    valid = check(!parser::parse_config(config, 0x80809C36U, 0x80810001U, stateVar, error),
                  "wrong projection array class was accepted")
            && valid;
    put(config, 708, 0x80800009U);
    valid = check(!parser::parse_config(config, 0x80809C35U, 0x80810001U, stateVar, error),
                  "wrong StateVar config class was accepted")
            && valid;
    return valid ? 0 : 1;
}
