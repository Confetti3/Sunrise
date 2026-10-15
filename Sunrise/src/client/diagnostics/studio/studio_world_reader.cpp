#include "studio_world_reader.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include "../../executable/image.h"
#include "../../hooks/world_objects/internal.h"
#include "../../memory/current_process_memory.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/registry.h"
#include "../../patterns/signature_text.h"
#include "studio_pose_scale.h"

namespace sunrise::client::diagnostics::studio {
namespace {

using patterns::signature;
using patterns::signature_length;
namespace objects = hooks::world_objects;

/** The loaded-name comparator supplies the loader getter and guarded pointer decoder. */
constexpr std::string_view kNameText =
    "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? 48 85 C0 74 ? "
    "48 8D 8B 10 01 00 00 41 B8 28 00 00 00";
/** The directive matcher supplies map, manager, current-slice and bubble-hash getters. */
constexpr std::string_view kBubbleText =
    "40 53 48 83 EC 20 81 39 C5 9D 1C 81 48 8B D9 74 ? 48 8D 4C 24 30 E8 ? ? ? ? "
    "8B 10 39 13 0F 94 C0 84 C0 74 ? E8 ? ? ? ? 48 8B C8 48 8D 54 24 30 E8 ? ? ? ? "
    "81 7C 24 30 FF 01 00 00 77 ? E8 ? ? ? ? 44 8B 44 24 30 48 8D 54 24 38 48 8B C8 "
    "E8 ? ? ? ? 8B 4B 04 39 08 0F 94 C0 48 83 C4 20 5B C3";
/** The slice residency getter keeps loading and resident states separate. */
constexpr std::string_view kResidencyText =
    "48 89 5C 24 10 44 89 44 24 18 57 48 83 EC 20 48 8B 59 08 48 8B FA 48 85 DB "
    "0F 84 ? ? ? ? 48 89 5C 24 30";
/** This getter accepts datum+96 and decodes the composed position at datum+208. */
constexpr std::string_view kPositionText =
    "40 57 48 83 EC 40 48 83 C1 A0 B8 00 00 00 00 48 8B FA F6 41 04 01 48 0F 44 C1 "
    "48 85 C0 74 ? 48 89 5C 24 50 0F 29 74 24 30 0F 10 B0 D0 00 00 00";
/** The current activity object retains the scenario-client handle accepted by world loading. */
constexpr std::string_view kScenarioText =
    "40 53 48 83 EC 20 48 8B D9 C7 01 FF FF FF FF E8 ? ? ? ? 48 85 C0 74 ? "
    "8B 40 48 89 03 48 8B C3 48 83 C4 20 5B C3";
constexpr auto kName = signature<signature_length(kNameText)>(kNameText);
constexpr auto kBubble = signature<signature_length(kBubbleText)>(kBubbleText);
constexpr auto kResidency = signature<signature_length(kResidencyText)>(kResidencyText);
constexpr auto kPosition = signature<signature_length(kPositionText)>(kPositionText);
constexpr auto kScenario = signature<signature_length(kScenarioText)>(kScenarioText);
constexpr std::array kPatterns{
    patterns::Pattern{"studio_loaded_name", kName},
    patterns::Pattern{"studio_bubble_readers", kBubble},
    patterns::Pattern{"studio_residency", kResidency},
    patterns::Pattern{"studio_composed_position", kPosition},
    patterns::Pattern{"studio_current_scenario", kScenario},
};

/** These call offsets belong to the exact caller signatures above. */
enum class CallSite : std::size_t {
    loader = 9,
    decode = 17,
    map = 22,
    manager = 38,
    current = 51,
    bubble = 84,
};
/** Offsets of the generation-checked datum's native identity and composed pose fields. */
constexpr std::size_t kComposedOrientationOffset = 192, kObjectViewOffset = 96;
constexpr std::size_t kParentHandleOffset = 60;
/** The residency query separates loaded content from the current instantiated slice. */
constexpr std::int32_t kResident = 3, kCurrent = 4;
constexpr std::uint32_t kEmptyHash = 0x811C9DC5U;

struct NativeObjectDatum final {
    std::array<std::byte, 4> prefix{};
    std::uint32_t flags{};
    std::uint32_t reserved8{};
    std::uint32_t selfHandle{};
    std::array<std::byte, 44> reserved16{};
    std::uint32_t parentHandle{};
    std::array<std::byte, 32> reserved64{};
    std::array<std::byte, 40> objectView{};
    std::uint32_t objectListTag{};
    std::uint32_t entryIndex{};
    std::uint64_t placementIdentity{};
    std::array<std::byte, 40> reserved152{};
    std::array<float, 4> composedOrientation{};
    std::array<std::uint32_t, 4> maskedPosition{};
};
static_assert(offsetof(NativeObjectDatum, composedOrientation) == kComposedOrientationOffset);
static_assert(offsetof(NativeObjectDatum, objectView) == kObjectViewOffset);
static_assert(offsetof(NativeObjectDatum, parentHandle) == kParentHandleOffset);
static_assert(sizeof(NativeObjectDatum) == kObjectDatumBytes);

/** The head of an ancestor datum: flags, own handle and parent handle. */
constexpr std::size_t kSelfHandleOffset = 12, kDatumLinkBytes = 64;
struct NativeDatumLink final {
    std::array<std::byte, 4> prefix{};
    std::uint32_t flags{};
    std::uint32_t reserved8{};
    std::uint32_t selfHandle{};
    std::array<std::byte, 44> reserved16{};
    std::uint32_t parentHandle{};
};
static_assert(offsetof(NativeDatumLink, selfHandle) == kSelfHandleOffset);
static_assert(offsetof(NativeDatumLink, parentHandle) == kParentHandleOffset);
static_assert(sizeof(NativeDatumLink) == kDatumLinkBytes);
/** A parent chain longer than this is treated as unreadable; it guards against a cycle. */
constexpr std::size_t kChainDepth = 32;

/** The manager retains the active map root's definition handle in its first field. */
struct NativeSliceIdentity final {
    std::uint32_t mapDefinitionTag{};
};

/** The loader swaps this complete record before publishing the selected root to the world. */
constexpr std::size_t kLoadedMapRootOffset = 316;
struct NativeActivityRecord final {
    std::array<char, 256> name{};
    std::array<std::byte, 60> reserved{};
    std::uint32_t mapRootTag{};
};
static_assert(offsetof(NativeActivityRecord, mapRootTag) == kLoadedMapRootOffset);

using Getter = void*(__fastcall*)();
using Decode = void*(__fastcall*)(void*);
using MapHash = std::uint32_t*(__fastcall*)(std::uint32_t*);
using CurrentSlice = void*(__fastcall*)(void*, std::int32_t*);
using SliceHash = std::uint32_t*(__fastcall*)(void*, std::uint32_t*, std::uint32_t);
using Residency = std::int32_t*(__fastcall*)(void*, std::int32_t*, std::uint32_t);
using Position = void(__fastcall*)(std::uintptr_t, void*);

struct Targets final {
    Getter loader{};
    Decode decode{};
    Getter manager{};
    MapHash map{};
    CurrentSlice current{};
    SliceHash bubble{};
    Residency residency{};
    Position position{};
    MapHash scenario{};
};
Targets g_targets{};

/** Decodes the near call at one site; every site is a fixed E8 in its caller signature. */
[[nodiscard]] std::byte* call_target(const std::byte* caller, CallSite site) noexcept {
    if (caller == nullptr) {
        return nullptr;
    }
    const std::byte* const instruction = caller + static_cast<std::size_t>(site);
    return patterns::resolve_relative(instruction + 1, instruction + 5);
}

/** A fault during world teardown fails only this read. */
template <typename T> [[nodiscard]] bool copy(std::uintptr_t address, T& output) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    return memory::copy_current_process(
        nullptr, address, std::as_writable_bytes(std::span(&output, 1)));
}

/** Native faults during world teardown invalidate this sample instead of escaping the frame. */
[[nodiscard]] bool sample_world(WorldSnapshot& output) noexcept {
    __try {
        void* const manager = g_targets.manager();
        void* const inner = manager == nullptr ? nullptr : g_targets.decode(manager);
        NativeSliceIdentity identity{};
        if (inner == nullptr || !copy(reinterpret_cast<std::uintptr_t>(inner), identity)
            || identity.mapDefinitionTag == objects::kNone || identity.mapDefinitionTag == 0) {
            return true;
        }
        output.mapDefinitionTag = identity.mapDefinitionTag;
        g_targets.current(manager, &output.currentSlice);
        if (output.currentSlice < 0
            || output.currentSlice >= static_cast<std::int32_t>(kNativeSliceCapacity)) {
            output.currentSlice = -1;
        }
        g_targets.map(&output.mapNameHash);
        if (g_targets.scenario != nullptr) {
            std::uint32_t scenario = objects::kNone;
            g_targets.scenario(&scenario);
            if (scenario != objects::kNone) {
                output.scenarioTag = scenario;
            }
        }
        void* const loader = g_targets.loader();
        void* const name = loader == nullptr ? nullptr : g_targets.decode(loader);
        NativeActivityRecord activity{};
        if (name != nullptr) {
            if (copy(reinterpret_cast<std::uintptr_t>(name), activity)) {
                std::copy_n(
                    activity.name.begin(), output.activityName.size(), output.activityName.begin());
                if (activity.mapRootTag != objects::kNone) {
                    output.mapRootTag = activity.mapRootTag;
                }
            }
            output.activityName.back() = '\0';
        }
        for (std::uint32_t index = 0; index < kNativeSliceCapacity; ++index) {
            std::int32_t state = 0;
            g_targets.residency(manager, &state, index);
            if (state != kResident && state != kCurrent) {
                continue;
            }
            std::uint32_t hash = kEmptyHash;
            g_targets.bubble(manager, &hash, index);
            if (hash != kEmptyHash) {
                output.loaded[output.loadedCount++] = {
                    hash, static_cast<std::uint16_t>(index), static_cast<std::uint8_t>(state)};
            }
        }
        NativeSliceIdentity after{};
        NativeActivityRecord afterActivity{};
        void* const finalInner = g_targets.decode(manager);
        output.present = finalInner == inner
                         && copy(reinterpret_cast<std::uintptr_t>(finalInner), after)
                         && after.mapDefinitionTag == identity.mapDefinitionTag;
        if (name != nullptr) {
            output.present = output.present && g_targets.decode(loader) == name
                             && copy(reinterpret_cast<std::uintptr_t>(name), afterActivity)
                             && activity.mapRootTag == afterActivity.mapRootTag
                             && activity.name == afterActivity.name;
        }
        if (g_targets.scenario != nullptr) {
            std::uint32_t scenario = objects::kNone;
            g_targets.scenario(&scenario);
            output.present =
                output.present && (scenario == objects::kNone ? 0 : scenario) == output.scenarioTag;
        }
        return output.present;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct PlacementAccess final {
    objects::ValidatePair validate{};
    const std::uintptr_t* base{};
    const std::uint32_t* stride{};
};

/** Copy stable accessor addresses before native calls so registry writers never wait for them. */
[[nodiscard]] bool placement_access(PlacementAccess& access) noexcept {
    if (!TryAcquireSRWLockShared(&objects::g_lock)) {
        return false;
    }
    const bool accepting = objects::g_accepting.load(std::memory_order_acquire);
    access = {objects::g_validatePair, objects::g_datumBaseStorage, objects::g_datumStrideStorage};
    ReleaseSRWLockShared(&objects::g_lock);
    return accepting && access.validate != nullptr && access.base != nullptr
           && access.stride != nullptr;
}

/** A mismatch proves the handle no longer names this object; unreadable proves nothing. */
enum class Validation : std::uint8_t { valid, mismatch, unreadable };

/** The datum slot a handle names; it does not check that the slot still holds that handle. */
[[nodiscard]] bool
datum_address(const PlacementAccess& access, std::uint32_t handle, std::uintptr_t& datum) noexcept {
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!copy(reinterpret_cast<std::uintptr_t>(access.base), base)
        || !copy(reinterpret_cast<std::uintptr_t>(access.stride), stride) || base == 0
        || stride < sizeof(NativeObjectDatum)) {
        return false;
    }
    datum = base + static_cast<std::uintptr_t>(stride) * (handle & objects::kEntityIndexMask);
    return true;
}

/** Full handles, generations and package tuples must survive both sides of a transform read. */
[[nodiscard]] Validation validate(const objects::Instance& identity,
                                  const PlacementAccess& access,
                                  std::uintptr_t& datum) noexcept {
    __try {
        const objects::HandlePair pair{identity.generation, identity.handle};
        std::int32_t handle = -1;
        access.validate(&pair, &handle);
        if (static_cast<std::uint32_t>(handle) != identity.handle) {
            return Validation::mismatch;
        }
        if (!datum_address(access, identity.handle, datum)) {
            return Validation::unreadable;
        }
        objects::DatumIdentity current{};
        if (!copy(datum, current)) {
            return Validation::unreadable;
        }
        return current.selfHandle == identity.handle
                       && current.objectListTag == identity.objectListTag
                       && current.entryIndex == identity.entryIndex
                       && (identity.placementIdentity == 0
                           || current.placementIdentity == identity.placementIdentity)
                   ? Validation::valid
                   : Validation::mismatch;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Validation::unreadable;
    }
}

/**
 * Walks from one datum to its root, combining every datum's flags.
 * @param output Receives root handle, root flags and chain flags; `stateKnown` on success.
 */
void read_chain(const PlacementAccess& access,
                const NativeObjectDatum& self,
                PlacementSnapshot& output) noexcept {
    std::uint32_t handle = self.selfHandle;
    std::uint32_t flags = self.flags, parent = self.parentHandle, chain = self.flags;
    for (std::size_t depth = 0; parent != objects::kNone; ++depth) {
        NativeDatumLink link{};
        std::uintptr_t datum = 0;
        if (depth == kChainDepth || !datum_address(access, parent, datum) || !copy(datum, link)
            || link.selfHandle != parent) {
            return;
        }
        handle = parent;
        flags = link.flags;
        parent = link.parentHandle;
        chain |= link.flags;
    }
    output.rootHandle = handle;
    output.rootFlags = flags;
    output.chainFlags = chain;
    output.stateKnown = true;
    output.placedNotHidden = is_placed_and_not_hidden(self.flags, flags, chain);
}

/** The native position getter reads its cache; it never runs the transform producer. */
[[nodiscard]] bool composed_position(std::uintptr_t datum, std::array<float, 4>& output) noexcept {
    __try {
        g_targets.position(datum + offsetof(NativeObjectDatum, objectView), output.data());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** @return True when every reader that read_world needs has resolved. */
[[nodiscard]] bool readers_ready() noexcept {
    return g_targets.loader != nullptr && g_targets.decode != nullptr
           && g_targets.manager != nullptr && g_targets.map != nullptr
           && g_targets.current != nullptr && g_targets.bubble != nullptr
           && g_targets.residency != nullptr;
}

} // namespace

/** Resolve independent readers in one executable-section scan. */
bool initialize_world_reader() noexcept {
    clear_world_reader();
    executable::ExecutableImage image{};
    if (!executable::inspect_main_module(image)) {
        return false;
    }
    std::array<patterns::ImageRange, executable::kPeSectionLimit> ranges{};
    for (std::size_t i = 0; i < image.count; ++i) {
        ranges[i].bytes = image.sections[i];
    }
    std::array<patterns::Match, kPatterns.size()> matches{};
    (void)patterns::resolve_all(std::span(ranges.data(), image.count), kPatterns, matches);
    const auto unique = [&](std::size_t index) -> std::byte* {
        return matches[index].status == patterns::MatchStatus::unique ? matches[index].address
                                                                      : nullptr;
    };
    g_targets.loader = reinterpret_cast<Getter>(call_target(unique(0), CallSite::loader));
    g_targets.decode = reinterpret_cast<Decode>(call_target(unique(0), CallSite::decode));
    g_targets.map = reinterpret_cast<MapHash>(call_target(unique(1), CallSite::map));
    g_targets.manager = reinterpret_cast<Getter>(call_target(unique(1), CallSite::manager));
    g_targets.current = reinterpret_cast<CurrentSlice>(call_target(unique(1), CallSite::current));
    g_targets.bubble = reinterpret_cast<SliceHash>(call_target(unique(1), CallSite::bubble));
    g_targets.residency = reinterpret_cast<Residency>(unique(2));
    g_targets.position = reinterpret_cast<Position>(unique(3));
    g_targets.scenario = reinterpret_cast<MapHash>(unique(4));
    return readers_ready();
}

void clear_world_reader() noexcept {
    g_targets = {};
}

/** Empty worlds and interrupted transitions never retain the preceding world's labels. */
WorldSnapshot read_world() noexcept {
    WorldSnapshot output{};
    output.available = readers_ready();
    if (output.available && !sample_world(output)) {
        output = {};
        output.available = true;
    }
    return output;
}

/**
 * Both identity lanes use the same generation-checked native pose cache.
 * @return True when the pose was read. On false, `output.gone` is set only for a gone object.
 */
[[nodiscard]] static bool read_object(const objects::Instance& identity,
                                      PlacementSnapshot& output,
                                      bool dynamic,
                                      std::array<std::byte, kObjectDatumBytes>* raw) noexcept {
    output = {};
    if (g_targets.position == nullptr || identity.handle == objects::kNone
        || (identity.objectListTag == objects::kNone) != dynamic
        || identity.entryIndex == objects::kNone) {
        return false;
    }
    PlacementAccess access{};
    std::uintptr_t before = 0, after = 0;
    if (!placement_access(access)) {
        return false;
    }
    const Validation first = validate(identity, access, before);
    NativeObjectDatum native{};
    if (first != Validation::valid || !copy(before, native)) {
        output.gone = first == Validation::mismatch;
        return false;
    }
    output.identity = identity;
    output.datumFlags = native.flags;
    output.parentHandle = native.parentHandle;
    output.flagsValid = true;
    output.gone = (native.flags & datumDestroyed) != 0;
    if (raw != nullptr) {
        std::memcpy(raw->data(), &native, sizeof native);
    }
    if (output.gone) {
        return false;
    }
    read_chain(access, native, output);
    // The position getter rejects an unbuilt datum; any other state still has a pose.
    NativeObjectDatum finalNative{};
    alignas(16) std::array<float, 4> position{};
    if ((native.flags & datumUnbuilt) != 0 || !composed_position(before, position)
        || !copy(before, finalNative) || (finalNative.flags & (datumUnbuilt | datumDestroyed)) != 0
        || native.composedOrientation != finalNative.composedOrientation
        || native.maskedPosition != finalNative.maskedPosition) {
        return false;
    }
    const Validation last = validate(identity, access, after);
    if (last != Validation::valid || before != after) {
        output.gone = last == Validation::mismatch;
        return false;
    }
    const float scale = decode_composed_scale(native.maskedPosition, position[2]);
    const auto finite = [](float value) { return std::isfinite(value); };
    if (!std::all_of(position.begin(), position.end(), finite) || !std::isfinite(scale)
        || !std::all_of(
            native.composedOrientation.begin(), native.composedOrientation.end(), finite)) {
        return false;
    }
    output.orientation = native.composedOrientation;
    output.scale = scale;
    std::copy_n(position.begin(), output.position.size(), output.position.begin());
    return true;
}

bool read_placement(const objects::Instance& identity,
                    PlacementSnapshot& output,
                    std::array<std::byte, kObjectDatumBytes>* raw) noexcept {
    return read_object(identity, output, false, raw);
}

bool read_dynamic(const objects::DynamicInstance& identity,
                  PlacementSnapshot& output,
                  std::array<std::byte, kObjectDatumBytes>* raw) noexcept {
    return read_object(identity.identity, output, true, raw);
}

} // namespace sunrise::client::diagnostics::studio
