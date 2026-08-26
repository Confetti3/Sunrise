#include "statics_footprints.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/statics_reader.h"
#include "../activity/activity_logic_packages.h"
#include "../activity/activity_graph_packages.h"
#include "../bubbles/bubble_bounds_packages.h"
#include "statics_footprint_epoch.h"

namespace sunrise::client::content::statics {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

constexpr std::size_t kTransformCapacity = 2048;
constexpr std::size_t kMeshCapacity = 64;
constexpr std::size_t kGroupInstanceCap = 256;

enum class Phase : std::uint8_t { idle, reading, done };

struct Request final {
    std::uint64_t activitySession{};
    std::uint32_t scenarioTag{};
    std::uint64_t epoch{};
    std::string mapFamily;
    std::wstring packageDirectory;
    reader::BlockKeys keys{};
    std::array<std::uint16_t, 8> packageIds{};
    std::size_t packageCount{};
    std::array<std::byte, 32> contentFingerprint{};
    RequestMode mode{RequestMode::liveLocation};
};

struct KeyScrubber final {
    reader::BlockKeys* keys{};
    ~KeyScrubber() noexcept {
        if (keys != nullptr) {
            SecureZeroMemory(keys, sizeof *keys);
        }
    }
};

struct Pass final {
    std::vector<std::uint32_t> tags;
    std::vector<Footprint> rows;
    std::vector<tables::StaticsTransform> transforms;
    std::vector<std::byte> blob;
    std::vector<std::byte> mesh;
    std::uint64_t epoch{};
    Phase phase{Phase::idle};
    Progress progress{};
    inspection::activity_logic_catalog::Catalog logic;
    activity::logic_packages::Progress logicProgress{};
    bool logicReady{};
    inspection::activity_catalog::Catalog graph;
    activity::graph_packages::Progress graphProgress{};
    bool graphReady{};
    inspection::bubble_catalog::Catalog bubbles;
    bubbles::packages::Progress bubbleProgress{};
    bool bubblesReady{};
    bool allocationFailure{};
};

struct Worker final {
    std::mutex commandMutex;
    std::condition_variable wake;
    std::thread thread;
    std::optional<Request> pending;
    RequestEpochs epochs;
    std::uint64_t currentSession{};
    std::string currentMap;
    std::array<std::uint16_t, 8> currentPackages{};
    std::size_t currentPackageCount{};
    std::uint32_t currentScenarioTag{};
    RequestMode currentMode{RequestMode::liveLocation};
    bool stopRequested{};
    bool initialized{};
    // Reader scratch is owned by the worker and never shared with the UI thread.
    reader::Scratch readerScratch{};
};

Worker g_worker;
std::mutex g_lifecycleMutex;
std::mutex g_publishMutex;
std::vector<Footprint> g_footprints;
inspection::activity_logic_catalog::Catalog g_logic;
activity::logic_packages::Progress g_logicProgress{};
bool g_logicReady{};
inspection::activity_catalog::Catalog g_graph;
activity::graph_packages::Progress g_graphProgress{};
bool g_graphReady{};
inspection::bubble_catalog::Catalog g_bubbles;
bubbles::packages::Progress g_bubbleProgress{};
bool g_bubblesReady{};
Progress g_progress{};
std::uint64_t g_publishedActivitySession{};
std::uint64_t g_completionActivitySession{};
std::uint32_t g_completionScenarioTag{};
RequestMode g_completionMode{RequestMode::liveLocation};
std::uint64_t g_publicationRevision{};
bool g_ready{false};
bool g_completionReady{false};
bool g_running{false};

bool log_line(const char* format, ...) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    va_list arguments{};
    va_start(arguments, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, arguments);
    va_end(arguments);
    if (written <= 0) {
        return false;
    }
    const std::size_t length = (std::min)(static_cast<std::size_t>(written), line.size() - 1U);
    core::log::write(core::log::Channel::middleware, core::log::Level::info, {line.data(), length});
    return true;
}

[[nodiscard]] bool finite3(const std::array<float, 3>& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

[[nodiscard]] std::string_view normalized_family_view(std::string_view family) noexcept {
    constexpr std::string_view destination = "_destination";
    constexpr std::string_view freeroam = "_freeroam";
    if (family.ends_with(destination)) {
        family.remove_suffix(destination.size());
    } else if (family.ends_with(freeroam)) {
        family.remove_suffix(freeroam.size());
    }
    return family;
}

[[nodiscard]] std::string normalize_family(std::string_view family) {
    return std::string(normalized_family_view(family));
}

[[nodiscard]] bool cancelled(std::uint64_t epoch) noexcept {
    const std::lock_guard<std::mutex> guard(g_worker.commandMutex);
    return g_worker.epochs.cancelled(epoch, g_worker.stopRequested);
}

bool collect_tag(void* context, std::uint32_t tag) noexcept {
    auto* pass = static_cast<Pass*>(context);
    if (cancelled(pass->epoch)) {
        return false;
    }
    try {
        pass->tags.push_back(tag);
        return true;
    } catch (...) {
        pass->allocationFailure = true;
        return false;
    }
}

bool collection_footprint(const reader::Source& source,
                          reader::Scratch& scratch,
                          std::vector<tables::StaticsTransform>& transforms,
                          std::vector<std::byte>& mesh,
                          std::uint32_t tag,
                          std::span<const std::byte> blob,
                          Footprint& output) {
    std::array<tables::StaticsArray, tables::kStaticsArrayCapacity> arrays{};
    const std::size_t found = tables::find_all_statics_arrays(blob, arrays);
    const tables::StaticsArray* records = nullptr;
    const tables::StaticsArray* meshTags = nullptr;
    const tables::StaticsArray* groups = nullptr;
    for (std::size_t index = 0; index < found; ++index) {
        if (arrays[index].elementClass == tables::kStaticsInstanceRecordsClass) {
            records = &arrays[index];
        } else if (arrays[index].elementClass == tables::kStaticsMeshTagsClass) {
            meshTags = &arrays[index];
        } else if (arrays[index].elementClass == tables::kStaticsGroupsClass) {
            groups = &arrays[index];
        }
    }
    if (records == nullptr || meshTags == nullptr || groups == nullptr
        || records->count > kTransformCapacity || groups->count > kMeshCapacity) {
        return false;
    }
    transforms.clear();
    transforms.resize(static_cast<std::size_t>(records->count));
    const tables::StaticsParse parsed =
        tables::parse_statics_transforms(blob, *records, transforms);
    if (!parsed.valid || parsed.parsed != transforms.size()) {
        return false;
    }
    tables::StaticsAabb footprint{};
    footprint.minimum.fill(3.4e38F);
    footprint.maximum.fill(-3.4e38F);
    std::size_t placed = 0;
    std::size_t meshes = 0;
    for (std::size_t group = 0; group < static_cast<std::size_t>(groups->count); ++group) {
        tables::StaticsGroup row{};
        if (!tables::statics_group_at(blob, *groups, group, row)) {
            return false;
        }
        if (row.instanceCount == 0) {
            continue;
        }
        if (row.instanceCount > kGroupInstanceCap
            || row.meshIndex >= static_cast<std::size_t>(meshTags->count)
            || static_cast<std::size_t>(row.instanceStart) > transforms.size()
            || static_cast<std::size_t>(row.instanceCount)
                   > transforms.size() - row.instanceStart) {
            return false;
        }
        std::uint32_t meshTag = 0;
        if (!tables::statics_mesh_tag_at(blob, *meshTags, row.meshIndex, meshTag)) {
            return false;
        }
        std::uint32_t meshClass = 0;
        mesh.clear();
        if (!reader::read_tag(source, scratch, meshTag, mesh, meshClass)
            || meshClass != tables::kStaticsMeshExtentsClass) {
            return false;
        }
        tables::StaticsAabb local{};
        if (!tables::mesh_resource_extents(mesh, local)) {
            return false;
        }
        ++meshes;
        for (std::size_t instance = 0; instance < row.instanceCount; ++instance) {
            tables::StaticsAabb placedBox{};
            if (!tables::transform_statics_aabb(
                    local, transforms[row.instanceStart + instance], placedBox)) {
                return false;
            }
            for (std::size_t lane = 0; lane < 3; ++lane) {
                footprint.minimum[lane] =
                    (std::min)(footprint.minimum[lane], placedBox.minimum[lane]);
                footprint.maximum[lane] =
                    (std::max)(footprint.maximum[lane], placedBox.maximum[lane]);
            }
            ++placed;
        }
    }
    if (placed == 0 || !finite3(footprint.minimum) || !finite3(footprint.maximum)) {
        return false;
    }
    output.tag = tag;
    output.minimum = footprint.minimum;
    output.maximum = footprint.maximum;
    output.instanceCount = static_cast<std::uint32_t>(placed);
    output.meshCount = static_cast<std::uint32_t>(meshes);
    return true;
}

void update_progress(const Progress& progress) noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    g_progress = progress;
}

void run(Pass& pass, const Request& request) {
    pass.epoch = request.epoch;
    const bool fullCollection = request.mode == RequestMode::liveLocation;
    if (fullCollection) {
        pass.rows.reserve(kFootprintCapacity);
        pass.transforms.reserve(kTransformCapacity);
    }
    pass.blob.reserve(64U * 1024U);
    pass.mesh.reserve(64U * 1024U);
    const reader::Source source{request.packageDirectory, &request.keys};
    std::string contentFamily;
    const bool familyResolved =
        request.scenarioTag != 0
        && reader::content_family(request.packageDirectory,
                                  tables::package_of(request.scenarioTag),
                                  contentFamily);
    reader::ScanResult declaredScan{};
    reader::ScanResult familyScan{};
    const bool declaredScanned = !fullCollection || request.packageCount == 0
                                 || reader::scan_class_packages(
                                     request.packageDirectory,
                                     std::span{request.packageIds.data(), request.packageCount},
                                     tables::kStaticsTransformClass,
                                     &collect_tag,
                                     &pass,
                                     declaredScan);
    // The scenario package's exact content family bounds sibling map/activity packages. The
    // canonical UI family can also prefix unrelated retained destinations (for example Mercury's
    // Lost Woods and Trials packages), so it is not a safe package-scan boundary.
    const bool familyScanned =
        !fullCollection
        || (familyResolved
            && reader::scan_class_family(request.packageDirectory,
                                         contentFamily,
                                         tables::kStaticsTransformClass,
                                         &collect_tag,
                                         &pass,
                                         familyScan));
    const bool scanned = declaredScanned && familyScanned;
    if (!scanned) {
        if (pass.allocationFailure) {
            pass.progress.allocationFailure = true;
            update_progress(pass.progress);
        }
    } else {
        std::sort(pass.tags.begin(), pass.tags.end());
        pass.tags.erase(std::unique(pass.tags.begin(), pass.tags.end()), pass.tags.end());
        pass.phase = Phase::reading;
        log_line("ev=statics_pass result=collected matches=%zu packages=%zu",
                 declaredScan.matches + familyScan.matches,
                 declaredScan.packages + familyScan.packages);
        for (std::uint32_t tag : pass.tags) {
            if (cancelled(pass.epoch)) {
                break;
            }
            ++pass.progress.collections;
            pass.blob.clear();
            std::uint32_t classId = 0;
            if (!reader::read_tag(source, g_worker.readerScratch, tag, pass.blob, classId)
                || classId != tables::kStaticsTransformClass) {
                ++pass.progress.rejected;
                update_progress(pass.progress);
                continue;
            }
            Footprint row{};
            if (!collection_footprint(
                    source, g_worker.readerScratch, pass.transforms, pass.mesh, tag, pass.blob, row)) {
                ++pass.progress.rejected;
                update_progress(pass.progress);
                continue;
            }
            if (pass.rows.size() >= kFootprintCapacity) {
                ++pass.progress.truncated;
                update_progress(pass.progress);
                continue;
            }
            pass.rows.push_back(row);
            ++pass.progress.published;
            update_progress(pass.progress);
        }
    }
    const auto is_cancelled = [](void* context) noexcept {
        return cancelled(*static_cast<const std::uint64_t*>(context));
    };
    if (request.scenarioTag != 0) {
        if (fullCollection) {
            pass.bubblesReady = bubbles::packages::build(source,
                                                         g_worker.readerScratch,
                                                         request.scenarioTag,
                                                         request.mapFamily,
                                                         is_cancelled,
                                                         &pass.epoch,
                                                         pass.bubbles,
                                                         pass.bubbleProgress);
            log_line("ev=bubble_bounds_pass result=%s parents=%zu placements=%zu bounds=%zu hash_packages=%zu empty_dependencies=%zu rejected=%zu diagnostic=%s",
                     pass.bubblesReady ? "ready" : "unavailable",
                     pass.bubbleProgress.parents,
                     pass.bubbleProgress.placements,
                     pass.bubbleProgress.published,
                     pass.bubbleProgress.hashPackages,
                     pass.bubbleProgress.emptyDependencies,
                     pass.bubbleProgress.rejected,
                     pass.bubbleProgress.diagnostic.c_str());
        }
        pass.graphReady = activity::graph_packages::build(
            source,
            g_worker.readerScratch,
            request.scenarioTag,
            request.mapFamily,
            request.contentFingerprint,
            is_cancelled,
            &pass.epoch,
            pass.graph,
            pass.graphProgress);
        log_line("ev=activity_graph_pass result=%s activities=%zu graphs=%zu nodes=%zu states=%zu links=%zu rejected=%zu diagnostic=%s",
                 pass.graphReady ? "ready" : "unavailable",
                 pass.graphProgress.activities,
                 pass.graphProgress.graphs,
                 pass.graphProgress.nodes,
                 pass.graphProgress.states,
                 pass.graphProgress.links,
                 pass.graphProgress.rejected,
                 pass.graphProgress.diagnostic.c_str());
        pass.logicReady = activity::logic_packages::build(
            source,
            g_worker.readerScratch,
            request.scenarioTag,
            request.mapFamily,
            request.contentFingerprint,
            is_cancelled,
            &pass.epoch,
            pass.logic,
            pass.logicProgress);
        log_line("ev=activity_logic_pass result=%s resources=%zu definitions=%zu map_rows=%zu published_placements=%zu references=%zu rejected=%zu",
                 pass.logicReady ? "ready" : "unavailable",
                 pass.logicProgress.resources,
                 pass.logicProgress.definitions,
                 pass.logicProgress.mapRows,
                 pass.logicProgress.publishedPlacements,
                 pass.logicProgress.references,
                 pass.logicProgress.rejected);
    }
    reader::close_files(g_worker.readerScratch);
    if (!cancelled(pass.epoch)) {
        pass.phase = Phase::done;
    }
}

void publish(Pass& pass, const Request& request) noexcept {
    std::sort(pass.rows.begin(),
              pass.rows.end(),
              [](const Footprint& left, const Footprint& right) { return left.tag < right.tag; });
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    if (request.mode == RequestMode::liveLocation) {
        g_footprints = std::move(pass.rows);
        g_bubbles = std::move(pass.bubbles);
        g_bubbleProgress = std::move(pass.bubbleProgress);
        g_bubblesReady = pass.bubblesReady;
        g_progress = pass.progress;
    }
    g_logic = std::move(pass.logic);
    g_logicProgress = pass.logicProgress;
    g_logicReady = pass.logicReady;
    g_graph = std::move(pass.graph);
    g_graphProgress = std::move(pass.graphProgress);
    g_graphReady = pass.graphReady;
    if (request.mode == RequestMode::liveLocation) {
        g_publishedActivitySession = request.activitySession;
    }
    g_completionActivitySession = request.activitySession;
    g_completionScenarioTag = request.scenarioTag;
    g_completionMode = request.mode;
    ++g_publicationRevision;
    if (g_publicationRevision == 0) {
        g_publicationRevision = 1;
    }
    if (request.mode == RequestMode::liveLocation) {
        g_ready = true;
    }
    g_completionReady = true;
    g_running = false;
}

void worker_loop() noexcept {
    for (;;) {
        Request request{};
        {
            std::unique_lock<std::mutex> lock(g_worker.commandMutex);
            g_worker.wake.wait(
                lock, [] { return g_worker.stopRequested || g_worker.pending.has_value(); });
            if (g_worker.stopRequested) {
                break;
            }
            request = std::move(*g_worker.pending);
            SecureZeroMemory(&g_worker.pending->keys, sizeof g_worker.pending->keys);
            g_worker.pending.reset();
            g_worker.epochs.activate(request.epoch);
        }
        const KeyScrubber requestKeys{&request.keys};
        (void)requestKeys;
        try {
            Pass pass{};
            run(pass, request);
            std::unique_lock<std::mutex> command(g_worker.commandMutex);
            if (g_worker.epochs.publishable(
                    request.epoch, g_worker.stopRequested, g_worker.pending.has_value())
                && pass.phase == Phase::done) {
                publish(pass, request);
            } else {
                const std::lock_guard<std::mutex> guard(g_publishMutex);
                g_running = g_worker.pending.has_value();
                if (!g_running && pass.phase != Phase::done) {
                    g_completionReady = false;
                    if (request.mode == RequestMode::liveLocation) {
                        g_ready = false;
                    }
                }
            }
        } catch (...) {
            log_line("ev=statics_pass result=aborted epoch=%llu",
                     static_cast<unsigned long long>(request.epoch));
            const std::unique_lock<std::mutex> command(g_worker.commandMutex);
            const std::lock_guard<std::mutex> guard(g_publishMutex);
            g_progress.allocationFailure = true;
            g_running = g_worker.pending.has_value();
            if (!g_running) {
                g_completionReady = false;
                if (request.mode == RequestMode::liveLocation) {
                    g_ready = false;
                }
            }
        }
    }
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    g_running = false;
}

void clear_publication() noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    g_footprints.clear();
    g_logic = {};
    g_logicProgress = {};
    g_logicReady = false;
    g_graph = {};
    g_graphProgress = {};
    g_graphReady = false;
    g_bubbles = {};
    g_bubbleProgress = {};
    g_bubblesReady = false;
    g_progress = {};
    g_publishedActivitySession = 0;
    g_completionActivitySession = 0;
    g_completionScenarioTag = 0;
    g_completionMode = RequestMode::liveLocation;
    g_ready = false;
    g_completionReady = false;
    g_running = false;
}

} // namespace

void initialize() noexcept {
    const std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (g_worker.thread.joinable()) {
        g_worker.initialized = true;
        return;
    }
    {
        const std::lock_guard<std::mutex> command(g_worker.commandMutex);
        g_worker.stopRequested = false;
        g_worker.pending.reset();
        g_worker.epochs.reset_for_start();
        g_worker.currentSession = 0;
        g_worker.currentMap.clear();
        g_worker.currentPackages = {};
        g_worker.currentPackageCount = 0;
        g_worker.currentScenarioTag = 0;
        g_worker.currentMode = RequestMode::liveLocation;
        g_worker.initialized = true;
    }
    try {
        g_worker.thread = std::thread(&worker_loop);
    } catch (...) {
        g_worker.initialized = false;
    }
}

void cancel() noexcept {
    {
        const std::lock_guard<std::mutex> guard(g_worker.commandMutex);
        g_worker.epochs.cancel();
        if (g_worker.pending.has_value()) {
            SecureZeroMemory(&g_worker.pending->keys, sizeof g_worker.pending->keys);
            g_worker.pending.reset();
        }
        g_worker.currentSession = 0;
        g_worker.currentMap.clear();
        g_worker.currentPackages = {};
        g_worker.currentPackageCount = 0;
        g_worker.currentScenarioTag = 0;
        g_worker.currentMode = RequestMode::liveLocation;
    }
    g_worker.wake.notify_one();
}

void shutdown() noexcept {
    std::thread thread;
    {
        const std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
        {
            const std::lock_guard<std::mutex> command(g_worker.commandMutex);
            g_worker.epochs.cancel();
            if (g_worker.pending.has_value()) {
                SecureZeroMemory(&g_worker.pending->keys, sizeof g_worker.pending->keys);
                g_worker.pending.reset();
            }
            g_worker.stopRequested = true;
        }
        g_worker.wake.notify_one();
        thread = std::move(g_worker.thread);
        g_worker.initialized = false;
        if (thread.joinable()) {
            thread.join();
        }
    }
    clear_publication();
}

void clear() noexcept {
    cancel();
    clear_publication();
}

bool ready() noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    return g_ready;
}

bool running() noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    return g_running;
}

void request_or_start(std::uint64_t activitySession,
                      std::uint32_t scenarioTag,
                      std::string_view mapFamily,
                      std::wstring_view packageDirectory,
                      const reader::BlockKeys& keys,
                      std::span<const std::uint16_t> packageIds,
                      std::span<const std::byte> contentFingerprint,
                      RequestMode mode) noexcept {
    if (activitySession == 0 || mapFamily.empty() || packageDirectory.empty()
        || (scenarioTag == 0 ? !contentFingerprint.empty() : contentFingerprint.size() != 32)) {
        return;
    }
    try {
        const std::string normalized = normalize_family(mapFamily);
        if (normalized.empty()) {
            return;
        }
        if (packageIds.size() > g_worker.currentPackages.size()) {
            return;
        }
        std::array<std::uint16_t, 8> normalizedPackages{};
        std::copy(packageIds.begin(), packageIds.end(), normalizedPackages.begin());
        std::sort(normalizedPackages.begin(), normalizedPackages.begin() + packageIds.size());
        const auto uniqueEnd = std::unique(normalizedPackages.begin(),
                                           normalizedPackages.begin() + packageIds.size());
        const std::size_t normalizedPackageCount =
            static_cast<std::size_t>(uniqueEnd - normalizedPackages.begin());
        initialize();
        std::unique_lock<std::mutex> command(g_worker.commandMutex);
        if (!g_worker.thread.joinable()) {
            return;
        }
        if (g_worker.currentSession == activitySession && g_worker.currentMap == normalized
            && g_worker.currentScenarioTag == scenarioTag && g_worker.currentMode == mode
            && g_worker.currentPackageCount == normalizedPackageCount
            && std::equal(normalizedPackages.begin(),
                          normalizedPackages.begin() + normalizedPackageCount,
                          g_worker.currentPackages.begin())) {
            return;
        }
        Request request{};
        request.activitySession = activitySession;
        request.scenarioTag = scenarioTag;
        request.epoch = g_worker.epochs.issue();
        request.mapFamily = normalized;
        request.packageDirectory.assign(packageDirectory.data(), packageDirectory.size());
        request.keys = keys;
        const KeyScrubber requestKeys{&request.keys};
        (void)requestKeys;
        request.packageIds = normalizedPackages;
        request.packageCount = normalizedPackageCount;
        std::copy(
            contentFingerprint.begin(), contentFingerprint.end(), request.contentFingerprint.begin());
        request.mode = mode;
        g_worker.currentSession = activitySession;
        g_worker.currentMap = normalized;
        g_worker.currentPackages = normalizedPackages;
        g_worker.currentPackageCount = normalizedPackageCount;
        g_worker.currentScenarioTag = scenarioTag;
        g_worker.currentMode = mode;
        if (g_worker.epochs.has_active()) {
            g_worker.epochs.cancel();
        }
        if (g_worker.pending.has_value()) {
            SecureZeroMemory(&g_worker.pending->keys, sizeof g_worker.pending->keys);
        }
        g_worker.pending = std::move(request);
        {
            const std::lock_guard<std::mutex> publication(g_publishMutex);
            if (mode == RequestMode::liveLocation) {
                g_ready = false;
                g_progress = {};
            }
            g_completionReady = false;
            g_running = true;
            g_completionActivitySession = 0;
            g_completionScenarioTag = 0;
        }
        command.unlock();
        g_worker.wake.notify_one();
    } catch (...) {
        // The current complete snapshot remains valid if a request cannot be queued.
    }
}

bool publish_cached(std::uint64_t activitySession,
                    std::span<const Footprint> rows,
                    const Progress& progress) noexcept {
    if (activitySession == 0 || rows.size() > kFootprintCapacity) {
        return false;
    }
    std::uint32_t previousTag = 0;
    for (const Footprint& row : rows) {
        if (row.tag == 0 || row.instanceCount == 0 || row.meshCount == 0
            || !finite3(row.minimum) || !finite3(row.maximum)
            || row.minimum[0] > row.maximum[0] || row.minimum[1] > row.maximum[1]
            || row.minimum[2] > row.maximum[2] || (previousTag != 0 && row.tag <= previousTag)) {
            return false;
        }
        previousTag = row.tag;
    }
    try {
        std::vector<Footprint> candidate(rows.begin(), rows.end());
        cancel();
        const std::lock_guard<std::mutex> guard(g_publishMutex);
        g_footprints = std::move(candidate);
        g_progress = progress;
        g_publishedActivitySession = activitySession;
        ++g_publicationRevision;
        if (g_publicationRevision == 0) {
            g_publicationRevision = 1;
        }
        g_ready = true;
        g_running = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool scope_matches(std::uint64_t activitySession) noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    return g_ready && activitySession != 0 && g_publishedActivitySession == activitySession;
}

bool publication_matches(std::uint64_t activitySession,
                         std::uint32_t scenarioTag,
                         RequestMode mode) noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    return g_completionReady && activitySession != 0 && scenarioTag != 0
           && g_completionActivitySession == activitySession
           && g_completionScenarioTag == scenarioTag && g_completionMode == mode;
}

std::uint64_t publication_revision() noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    return g_publicationRevision;
}

Progress progress() noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    return g_progress;
}

bool snapshot(std::span<Footprint> output, std::size_t& count) noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    count = g_footprints.size();
    if (output.size() < g_footprints.size()) {
        return false;
    }
    std::copy(g_footprints.begin(), g_footprints.end(), output.begin());
    return true;
}

bool logic_snapshot(inspection::activity_logic_catalog::Catalog& output,
                    activity::logic_packages::Progress& progress) noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    if (!g_logicReady) {
        output = {};
        progress = g_logicProgress;
        return false;
    }
    try {
        output = g_logic;
        progress = g_logicProgress;
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool graph_snapshot(inspection::activity_catalog::Catalog& output,
                    activity::graph_packages::Progress& progress) noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    if (!g_graphReady) {
        output = {};
        progress = g_graphProgress;
        return false;
    }
    try {
        output = g_graph;
        progress = g_graphProgress;
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool bubble_snapshot(inspection::bubble_catalog::Catalog& output,
                     bubbles::packages::Progress& progress) noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    if (!g_ready || !g_bubblesReady) {
        progress = g_bubbleProgress;
        return false;
    }
    try {
        output = g_bubbles;
        progress = g_bubbleProgress;
        return true;
    } catch (...) {
        output = {};
        progress = g_bubbleProgress;
        return false;
    }
}

} // namespace sunrise::client::content::statics
