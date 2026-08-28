#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "client/inspection/current_location_domain_cache.h"

namespace cache = sunrise::client::inspection::current_location_domain_cache;
namespace logic = sunrise::client::inspection::activity_logic_catalog;
namespace graphs = sunrise::client::inspection::activity_catalog;
namespace bubbles = sunrise::client::inspection::bubble_catalog;

namespace {

bool check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
    }
    return value;
}

template <std::size_t Size>
void copy_fingerprint(const std::array<std::byte, Size>& source,
                      std::array<std::uint8_t, Size>& destination) {
    std::transform(source.begin(), source.end(), destination.begin(), [](std::byte value) {
        return static_cast<std::uint8_t>(value);
    });
}

logic::Catalog logic_catalog(const std::array<std::byte, logic::kDigestSize>& fingerprint) {
    logic::Catalog value{};
    value.provenance.collectorVersion = logic::kCollectorVersion;
    value.provenance.contentBuild = 86657;
    copy_fingerprint(fingerprint, value.provenance.contentFingerprint);
    value.entities = {
        {0x80800001U, 1, 2, logic::Role::object, logic::Confidence::strong, "One", "one", "", {}},
        {0x80800003U,
         1,
         2,
         logic::Role::objective,
         logic::Confidence::strong,
         "Three",
         "three",
         "localized",
         {{7, 8, 9, {1.0F, 2.0F, 3.0F}, {0.0F, 0.0F, 0.0F, 1.0F}}}},
    };
    value.activities = {{0x80B3E142U, "Titan", "fleet", {0, 1}}};
    value.edges = {{0, 1, 0x1234U, 2}};
    value.stateVars = {{0x80810001U,
                        0x82E6ECA4U,
                        7,
                        -1,
                        1,
                        -1,
                        -1,
                        true,
                        3,
                        2,
                        "has_spawned",
                        true,
                        {{10, 20, 0, 0x80820001U}}}};
    value.stateVarBindings = {{0x80820002U, 0x80810001U, 0}};
    value.logicRoots = {{0x80830001U, 0x8080941EU, "logic root"}};
    value.logicReferences = {
        {0, 0, 0x82E6ECA4U, 2, 7, logic::LogicReferenceDirection::read},
        {0,
         logic::LogicReference::kUnjoinedStateVar,
         0x5678U,
         1,
         -1,
         logic::LogicReferenceDirection::write},
    };
    return value;
}

bubbles::Catalog bubble_catalog() {
    bubbles::Catalog value{};
    value.contentBuild = bubbles::kTargetContentBuild;
    value.family = "fleet";
    value.bubbles = {
        {1, {-1.0F, -2.0F, -3.0F}, {1.0F, 2.0F, 3.0F}, "fleet"},
        {2, {10.0F, 11.0F, 12.0F}, {13.0F, 14.0F, 15.0F}, "fleet"},
    };
    return value;
}

graphs::Catalog graph_catalog(
    std::uint32_t scenarioTag,
    const std::array<std::byte, graphs::kDigestSize>& fingerprint) {
    graphs::Catalog value{};
    value.contentBuild = graphs::kTargetContentBuild;
    value.collectorVersion = graphs::kCollectorVersion;
    value.scenarioTag = scenarioTag;
    copy_fingerprint(fingerprint, value.contentFingerprint);
    value.activities = {{0x12345678U, "activity_name", {0xABCDEF01U}}};
    graphs::GraphNode node{};
    node.graphHash = 0xABCDEF01U;
    node.nodeHash = 0x87654321U;
    node.authoredX = -12.0F;
    node.authoredY = 34.0F;
    node.stateValues = {0, 4, 4, 0};
    node.activityHashes = {0x12345678U};
    value.graphs = {{0xABCDEF01U, {std::move(node)}, {}}};
    return value;
}

void overwrite_u32(const std::wstring& path, std::streamoff offset, std::uint32_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(&value), sizeof value);
}

} // namespace

int main() {
    std::array<std::byte, graphs::kDigestSize> fingerprint{};
    for (std::size_t index = 0; index < fingerprint.size(); ++index) {
        fingerprint[index] = static_cast<std::byte>(index + 1U);
    }
    auto wrongFingerprint = fingerprint;
    wrongFingerprint[0] ^= std::byte{0xFF};

    std::array<wchar_t, MAX_PATH> root{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(root.size()), root.data());
    if (!check(length != 0 && length < root.size(), "temp path unavailable")) {
        return 1;
    }
    const std::wstring logicPath = std::wstring(root.data()) + L"sunrise-location-logic.bin";
    const std::wstring bubblePath = std::wstring(root.data()) + L"sunrise-location-bubbles.bin";
    const std::wstring graphPath = std::wstring(root.data()) + L"sunrise-location-graph.bin";
    DeleteFileW(logicPath.c_str());
    DeleteFileW(bubblePath.c_str());
    DeleteFileW(graphPath.c_str());

    bool valid = true;
    graphs::Catalog graph = graph_catalog(0x80B3E142U, fingerprint);
    std::string diagnostic;
    valid = check(cache::store_activity_graph_atomic(
                      graphPath, graph, 0x80B3E142U, fingerprint, diagnostic),
                  "graph cache write failed")
            && valid;
    graphs::Catalog loadedGraph;
    valid = check(cache::load_activity_graph(graphPath, 0x80B3E142U, fingerprint, loadedGraph).state
                      == cache::LoadState::ready
                      && loadedGraph.graphs.front().nodes.front().stateValues
                             == std::vector<std::uint32_t>({0, 4, 4, 0}),
                  "graph cache round trip failed")
            && valid;
    valid = check(cache::load_activity_graph(graphPath, 0x80F4696AU, fingerprint, loadedGraph).state
                      == cache::LoadState::rejected,
                  "graph cache accepted the wrong scenario")
            && valid;
    valid = check(cache::load_activity_graph(
                      graphPath, 0x80B3E142U, wrongFingerprint, loadedGraph).state
                      == cache::LoadState::rejected,
                  "graph cache accepted the wrong fingerprint")
            && valid;
    overwrite_u32(graphPath, 8, 2);
    valid = check(cache::load_activity_graph(graphPath, 0x80B3E142U, fingerprint, loadedGraph).state
                      == cache::LoadState::stale,
                  "graph schema 2 was not reported stale")
            && valid;
    diagnostic.clear();
    valid = check(cache::store_activity_graph_atomic(
                      graphPath, graph, 0x80B3E142U, fingerprint, diagnostic),
                  "graph cache did not replace stale schema")
            && valid;

    logic::Catalog logicValue = logic_catalog(fingerprint);
    diagnostic.clear();
    valid = check(cache::store_activity_logic_atomic(
                      logicPath, logicValue, 0x80B3E142U, fingerprint, diagnostic),
                  "logic cache write failed")
            && valid;
    logic::Catalog loadedLogic;
    valid = check(cache::load_activity_logic(
                      logicPath, 0x80B3E142U, fingerprint, loadedLogic).state
                      == cache::LoadState::ready
                      && loadedLogic.entities.size() == 2 && loadedLogic.edges.size() == 1
                      && loadedLogic.stateVars.size() == 1
                      && loadedLogic.stateVars.front().initial == 7
                      && loadedLogic.stateVars.front().triggers.size() == 1
                      && loadedLogic.stateVars.front().triggers.front().referenceTag == 0
                      && loadedLogic.stateVarBindings.size() == 1
                      && loadedLogic.stateVarBindings.front().ownerTag == 0x80820002U
                      && loadedLogic.stateVarBindings.front().configTag == 0x80810001U
                      && loadedLogic.logicRoots.size() == 1
                      && loadedLogic.logicReferences.size() == 2
                      && loadedLogic.logicReferences.front().selector == 7
                      && loadedLogic.logicReferences.back().stateVarIndex
                             == logic::LogicReference::kUnjoinedStateVar,
                  "logic cache round trip failed")
            && valid;
    valid = check(cache::load_activity_logic(
                      logicPath, 0x80F4696AU, fingerprint, loadedLogic).state
                      == cache::LoadState::rejected,
                  "logic cache accepted the wrong scenario")
            && valid;
    valid = check(cache::load_activity_logic(
                      logicPath, 0x80B3E142U, wrongFingerprint, loadedLogic).state
                      == cache::LoadState::rejected,
                  "logic cache accepted the wrong fingerprint")
            && valid;
    logic::Catalog invalidLogic = logicValue;
    invalidLogic.edges.front().occurrenceCount = 0;
    diagnostic.clear();
    valid = check(!cache::store_activity_logic_atomic(
                      logicPath, invalidLogic, 0x80B3E142U, fingerprint, diagnostic),
                  "zero-count logic edge replaced the valid file")
            && valid;
    invalidLogic = logicValue;
    invalidLogic.edges.push_back(invalidLogic.edges.front());
    diagnostic.clear();
    valid = check(!cache::store_activity_logic_atomic(
                      logicPath, invalidLogic, 0x80B3E142U, fingerprint, diagnostic),
                  "duplicate logic edge replaced the valid file")
            && valid;
    valid = check(cache::load_activity_logic(
                      logicPath, 0x80B3E142U, fingerprint, loadedLogic).state
                      == cache::LoadState::ready
                      && loadedLogic.edges.size() == 1,
                  "invalid logic candidate damaged the valid file")
            && valid;
    overwrite_u32(logicPath, 64 + 5 * 12, 0xFFFFFFF0U);
    valid = check(cache::load_activity_logic(
                      logicPath, 0x80B3E142U, fingerprint, loadedLogic).state
                      == cache::LoadState::rejected,
                  "malformed StateVar section range was accepted")
            && valid;
    diagnostic.clear();
    valid = check(cache::store_activity_logic_atomic(
                      logicPath, logicValue, 0x80B3E142U, fingerprint, diagnostic),
                  "logic cache did not recover from malformed StateVar section")
            && valid;
    overwrite_u32(logicPath, 60, logic::kCollectorVersion - 1U);
    valid = check(cache::load_activity_logic(
                      logicPath, 0x80B3E142U, fingerprint, loadedLogic).state
                      == cache::LoadState::stale,
                  "older logic collector was not reported stale")
            && valid;
    diagnostic.clear();
    valid = check(cache::store_activity_logic_atomic(
                      logicPath, logicValue, 0x80B3E142U, fingerprint, diagnostic),
                  "logic cache did not replace stale collector")
            && valid;
    overwrite_u32(logicPath, 8, 3);
    valid = check(cache::load_activity_logic(
                      logicPath, 0x80B3E142U, fingerprint, loadedLogic).state
                      == cache::LoadState::stale,
                  "logic schema 3 was not reported stale")
            && valid;
    diagnostic.clear();
    valid = check(cache::store_activity_logic_atomic(
                      logicPath, logicValue, 0x80B3E142U, fingerprint, diagnostic),
                  "logic cache did not replace stale schema")
            && valid;
    logicValue.provenance.contentBuild = 0;
    diagnostic.clear();
    valid = check(!cache::store_activity_logic_atomic(
                      logicPath, logicValue, 0x80B3E142U, fingerprint, diagnostic),
                  "wrong-build logic replaced the valid file")
            && valid;

    bubbles::Catalog bubblesValue = bubble_catalog();
    diagnostic.clear();
    valid = check(cache::store_bubble_bounds_atomic(
                      bubblePath, bubblesValue, "fleet", diagnostic),
                  "bubble cache write failed")
            && valid;
    bubbles::Catalog loadedBubbles;
    valid = check(cache::load_bubble_bounds(bubblePath, "fleet", loadedBubbles).state
                      == cache::LoadState::ready,
                  "bubble cache round trip failed")
            && valid;
    valid = check(cache::load_bubble_bounds(bubblePath, "mercury", loadedBubbles).state
                      == cache::LoadState::rejected,
                  "bubble cache accepted the wrong family")
            && valid;
    overwrite_u32(bubblePath, 8, 1);
    valid = check(cache::load_bubble_bounds(bubblePath, "fleet", loadedBubbles).state
                      == cache::LoadState::rejected,
                  "legacy bubble schema was accepted")
            && valid;

    {
        std::fstream corrupt(logicPath, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.put('X');
    }
    valid = check(cache::load_activity_logic(
                      logicPath, 0x80B3E142U, fingerprint, loadedLogic).state
                      == cache::LoadState::rejected,
                  "corrupt logic cache was accepted")
            && valid;

    DeleteFileW(logicPath.c_str());
    DeleteFileW(bubblePath.c_str());
    DeleteFileW(graphPath.c_str());
    return valid ? 0 : 1;
}
