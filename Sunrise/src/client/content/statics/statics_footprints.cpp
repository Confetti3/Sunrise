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

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/statics_reader.h"
#include "../items/packages/internal.h"
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
    std::uint64_t epoch{};
    std::string mapFamily;
    std::wstring packageDirectory;
    reader::BlockKeys keys{};
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
    bool stopRequested{};
    bool initialized{};
    // Reader scratch is owned by the worker and never shared with the UI thread.
    reader::Scratch readerScratch{};
};

Worker g_worker;
std::mutex g_lifecycleMutex;
std::mutex g_publishMutex;
std::vector<Footprint> g_footprints;
Progress g_progress{};
std::uint64_t g_publishedActivitySession{};
std::uint64_t g_publicationRevision{};
bool g_ready{false};
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
    pass.rows.reserve(kFootprintCapacity);
    pass.transforms.reserve(kTransformCapacity);
    pass.blob.reserve(64U * 1024U);
    pass.mesh.reserve(64U * 1024U);
    const reader::Source source{request.packageDirectory, &request.keys};
    reader::ScanResult scan{};
    if (!reader::scan_class_family(request.packageDirectory,
                                   request.mapFamily,
                                   tables::kStaticsTransformClass,
                                   &collect_tag,
                                   &pass,
                                   scan)) {
        if (pass.allocationFailure) {
            pass.progress.allocationFailure = true;
            update_progress(pass.progress);
        }
        reader::close_files(g_worker.readerScratch);
        return;
    }
    std::sort(pass.tags.begin(), pass.tags.end());
    pass.tags.erase(std::unique(pass.tags.begin(), pass.tags.end()), pass.tags.end());
    pass.phase = Phase::reading;
    log_line(
        "ev=statics_pass result=collected matches=%zu packages=%zu", scan.matches, scan.packages);
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
    reader::close_files(g_worker.readerScratch);
    pass.phase = Phase::done;
}

void publish(Pass& pass, const Request& request) noexcept {
    std::sort(pass.rows.begin(),
              pass.rows.end(),
              [](const Footprint& left, const Footprint& right) { return left.tag < right.tag; });
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    g_footprints = std::move(pass.rows);
    g_progress = pass.progress;
    g_publishedActivitySession = request.activitySession;
    ++g_publicationRevision;
    if (g_publicationRevision == 0) {
        g_publicationRevision = 1;
    }
    g_ready = true;
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
            g_worker.pending.reset();
            g_worker.epochs.activate(request.epoch);
        }
        try {
            Pass pass{};
            run(pass, request);
            std::unique_lock<std::mutex> command(g_worker.commandMutex);
            if (g_worker.epochs.publishable(
                    request.epoch, g_worker.stopRequested, g_worker.pending.has_value())) {
                publish(pass, request);
            } else {
                const std::lock_guard<std::mutex> guard(g_publishMutex);
                g_running = g_worker.pending.has_value();
            }
        } catch (...) {
            log_line("ev=statics_pass result=aborted epoch=%llu",
                     static_cast<unsigned long long>(request.epoch));
            const std::lock_guard<std::mutex> guard(g_publishMutex);
            g_progress.allocationFailure = true;
            g_running = false;
            g_ready = false;
        }
    }
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    g_running = false;
}

void clear_publication() noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    g_footprints.clear();
    g_progress = {};
    g_publishedActivitySession = 0;
    g_ready = false;
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
        g_worker.pending.reset();
        g_worker.currentSession = 0;
        g_worker.currentMap.clear();
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
            g_worker.pending.reset();
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
                      std::string_view mapFamily,
                      std::wstring_view packageDirectory,
                      const reader::BlockKeys& keys) noexcept {
    if (activitySession == 0 || mapFamily.empty() || packageDirectory.empty()) {
        return;
    }
    try {
        const std::string normalized = normalize_family(mapFamily);
        if (normalized.empty()) {
            return;
        }
        initialize();
        std::unique_lock<std::mutex> command(g_worker.commandMutex);
        if (!g_worker.thread.joinable()) {
            return;
        }
        if (g_worker.currentSession == activitySession && g_worker.currentMap == normalized) {
            return;
        }
        Request request{};
        request.activitySession = activitySession;
        request.epoch = g_worker.epochs.issue();
        request.mapFamily = normalized;
        request.packageDirectory.assign(packageDirectory.data(), packageDirectory.size());
        request.keys = keys;
        g_worker.currentSession = activitySession;
        g_worker.currentMap = normalized;
        if (g_worker.epochs.has_active()) {
            g_worker.epochs.cancel();
        }
        g_worker.pending = std::move(request);
        {
            const std::lock_guard<std::mutex> publication(g_publishMutex);
            g_ready = false;
            g_running = true;
            g_progress = {};
            g_publishedActivitySession = 0;
        }
        command.unlock();
        g_worker.wake.notify_one();
    } catch (...) {
        // The current complete snapshot remains valid if a request cannot be queued.
    }
}

void advance_pass(std::uint64_t activitySession, std::string_view mapFamily) noexcept {
    const std::string_view normalized = normalized_family_view(mapFamily);
    if (activitySession == 0 || normalized.empty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> command(g_worker.commandMutex);
        if (g_worker.currentSession == activitySession && g_worker.currentMap == normalized) {
            return;
        }
    }
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    // Copy native key material on the render thread, but defer every filesystem
    // query/open/read to the worker. Building the module-relative path is purely local.
    if (!items::packages::collect_keys(keys)
        || !core::path::module_directory(GetModuleHandleW(nullptr), directory)
        || !core::path::append(directory, L"packages")) {
        SecureZeroMemory(&keys, sizeof keys);
        return;
    }
    request_or_start(activitySession,
                     normalized,
                     std::wstring_view(directory.chars.data(), directory.length),
                     keys);
    SecureZeroMemory(&keys, sizeof keys);
}

bool scope_matches(std::uint64_t activitySession) noexcept {
    const std::lock_guard<std::mutex> guard(g_publishMutex);
    return g_ready && activitySession != 0 && g_publishedActivitySession == activitySession;
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

} // namespace sunrise::client::content::statics
