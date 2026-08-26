#include "current_location_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "../../core/filesystem/path.h"
#include "../../state/content_manifest/content_manifest_state_runtime.h"
#include "../content/items/packages/build.h"
#include "../content/items/packages/internal.h"
#include "../content/bubbles/bubble_bounds_packages.h"
#include "../content/statics/statics_footprint_cache.h"
#include "../content/statics/statics_footprints.h"
#include "current_location_domain_cache.h"
#include "providers/activity_logic_inspection.h"
#include "providers/activity_graph_inspection.h"
#include "providers/bubble_bounds_inspection.h"

namespace sunrise::client::inspection::current_location_catalog {
namespace {

namespace package_items = client::content::items::packages;
namespace statics = client::content::statics;
namespace statics_cache = client::content::statics::cache;
namespace logic_packages = client::content::activity::logic_packages;
namespace graph_packages = client::content::activity::graph_packages;
namespace bubble_packages = client::content::bubbles::packages;
namespace domain_cache = current_location_domain_cache;
namespace logic_provider = providers::activity_logic;
namespace graph_provider = providers::activity_graph;
namespace bubble_provider = providers::bubble_bounds;

core::path::Buffer g_artifactRoot{};
statics_cache::Key g_key{};
Scope g_scope{};
Status g_status{};
ActivitySelection g_preview{};
statics_cache::Key g_previewKey{};
PreviewStatus g_previewStatus{};
std::uint64_t g_seenStaticsRevision{};
std::uint64_t g_seenGraphRevision{};
std::uint64_t g_seenLogicRevision{};
std::uint64_t g_seenBubbleRevision{};
bool g_initialized{};
bool g_scopeSelected{};
bool g_collecting{};
statics::RequestMode g_requestMode{statics::RequestMode::liveLocation};

[[nodiscard]] Source activation_source(const Scope& scope, bool preview) {
    Source source{};
    source.packageName = scope.packageName;
    source.mapStem = scope.mapFamily;
    source.scenarioTag = scope.scenarioTag;
    source.authoredPreview = preview;
    if (!preview) {
        source.activitySession = scope.activitySession;
    }
    return source;
}

[[nodiscard]] std::string normalize_family(std::string_view family) {
    constexpr std::string_view destination = "_destination";
    constexpr std::string_view freeroam = "_freeroam";
    if (family.ends_with(destination)) {
        family.remove_suffix(destination.size());
    } else if (family.ends_with(freeroam)) {
        family.remove_suffix(freeroam.size());
    }
    return std::string(family);
}

bool copy_fingerprint(void* context, const state::content_manifest::View& view) noexcept {
    auto* output = static_cast<state::content_manifest::Fingerprint*>(context);
    std::copy(view.buildFingerprint.begin(), view.buildFingerprint.end(), output->begin());
    return true;
}

[[nodiscard]] bool make_key(const Scope& input,
                            Scope& normalized,
                            statics_cache::Key& key) noexcept {
    if (input.activitySession == 0 || input.scenarioTag == 0 || input.mapFamily.empty()
        || input.packageCount == 0 || input.packageCount > input.packageIds.size()) {
        return false;
    }
    try {
        normalized = input;
        normalized.mapFamily = normalize_family(input.mapFamily);
        if (normalized.mapFamily.empty() || normalized.mapFamily.size() >= 32) {
            return false;
        }
        std::sort(normalized.packageIds.begin(),
                  normalized.packageIds.begin() + normalized.packageCount);
        const auto uniqueEnd = std::unique(normalized.packageIds.begin(),
                                           normalized.packageIds.begin()
                                               + normalized.packageCount);
        normalized.packageCount =
            static_cast<std::size_t>(uniqueEnd - normalized.packageIds.begin());
        if (normalized.packageCount == 0 || normalized.packageIds[0] == 0) {
            return false;
        }
        for (std::size_t index = 0; index < normalized.packageCount; ++index) {
            if (normalized.packageIds[index] == 0) {
                return false;
            }
        }
        key = {};
        key.scenarioTag = normalized.scenarioTag;
        key.mapFamily = normalized.mapFamily;
        key.packageCount = normalized.packageCount;
        std::copy(normalized.packageIds.begin(),
                  normalized.packageIds.begin() + normalized.packageCount,
                  key.packageIds.begin());
        return state::content_manifest::visit_snapshot(&copy_fingerprint,
                                                       &key.contentFingerprint);
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool same_key(const statics_cache::Key& left,
                            const statics_cache::Key& right) noexcept {
    return left.scenarioTag == right.scenarioTag && left.mapFamily == right.mapFamily
           && left.packageCount == right.packageCount
           && left.contentFingerprint == right.contentFingerprint
           && std::equal(left.packageIds.begin(),
                         left.packageIds.begin() + left.packageCount,
                         right.packageIds.begin());
}

[[nodiscard]] bool directory(core::path::Buffer& path, std::wstring_view suffix, bool create) {
    if (!core::path::append(path, suffix)) {
        return false;
    }
    if (!create) {
        return true;
    }
    if (CreateDirectoryW(path.chars.data(), nullptr) != FALSE) {
        return true;
    }
    const DWORD attributes = GetFileAttributesW(path.chars.data());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

[[nodiscard]] bool cache_path(const statics_cache::Key& key,
                              std::wstring_view filename,
                              core::path::Buffer& path,
                              bool create) {
    path = g_artifactRoot;
    if (!directory(path, L"\\cache", create) || !directory(path, L"\\inspection", create)) {
        return false;
    }
    std::array<wchar_t, state::content_manifest::kFingerprintSize * 2 + 2> fingerprint{};
    std::size_t cursor = 0;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (const std::byte value : key.contentFingerprint) {
        const auto lane = static_cast<unsigned char>(value);
        fingerprint[cursor++] = digits[lane >> 4U];
        fingerprint[cursor++] = digits[lane & 0x0FU];
    }
    std::wstring fingerprintSuffix = L"\\";
    fingerprintSuffix.append(fingerprint.data(), cursor);
    if (!directory(path, fingerprintSuffix, create)) {
        return false;
    }
    std::array<wchar_t, 16> scenario{};
    const int written = std::swprintf(scenario.data(), scenario.size(), L"\\%08X", key.scenarioTag);
    return written > 0 && directory(path, scenario.data(), create)
           && core::path::append(path, filename);
}

void unavailable_domains() {
    g_status.activityGraph = {DomainState::idle, 0, {}};
    g_status.activityLogic = {DomainState::idle, 0, {}};
    g_status.bubbleBounds = {DomainState::idle, 0, {}};
}

[[nodiscard]] bool load_cache(const Scope& scope, const statics_cache::Key& key) {
    core::path::Buffer path{};
    if (!cache_path(key, L"\\statics-footprints.bin", path, false)) {
        g_status.statics = {DomainState::failed, 0, "The location cache path is unavailable."};
        return false;
    }
    std::vector<statics::Footprint> rows;
    statics::Progress progress{};
    const auto loaded = statics_cache::load(
        std::wstring_view(path.chars.data(), path.length), key, rows, progress);
    if (loaded.state == statics_cache::LoadState::ready
        && statics::publish_cached(scope.activitySession, rows, progress)) {
        g_status.statics = {DomainState::cached, rows.size(), loaded.diagnostic};
        g_seenStaticsRevision = statics::publication_revision();
        return true;
    }
    g_status.statics = {
        loaded.state == statics_cache::LoadState::rejected ? DomainState::failed
                                                            : DomainState::idle,
        0,
        loaded.diagnostic};
    return false;
}

[[nodiscard]] bool
load_domain_caches(const Scope& scope, const statics_cache::Key& key, bool preview = false) {
    bool publicationChanged = false;
    DomainStatus& graphStatus =
        preview ? g_previewStatus.activityGraph : g_status.activityGraph;
    DomainStatus& logicStatus =
        preview ? g_previewStatus.activityLogic : g_status.activityLogic;
    core::path::Buffer path{};
    activity_catalog::Catalog graph;
    const bool graphPath = cache_path(key, L"\\activity-graph.bin", path, false);
    const auto graphLoaded = graphPath
                                 ? domain_cache::load_activity_graph(
                                       std::wstring_view(path.chars.data(), path.length),
                                       scope.scenarioTag,
                                       key.contentFingerprint,
                                       graph)
                                 : domain_cache::LoadResult{
                                       domain_cache::LoadState::rejected,
                                       "The Activity Graph cache path is unavailable."};
    if (graphLoaded.state == domain_cache::LoadState::ready
        && graph_provider::activate_location(std::move(graph), activation_source(scope, preview))) {
        std::size_t count = 0;
        for (const auto& value : graph_provider::state().locationCatalog.graphs) {
            count += value.nodes.size();
        }
        graphStatus = {DomainState::cached, count, graphLoaded.diagnostic};
        publicationChanged = true;
    } else {
        graphStatus =
            graphLoaded.state == domain_cache::LoadState::rejected
                ? DomainStatus{DomainState::failed, 0, graphLoaded.diagnostic}
                : DomainStatus{DomainState::idle,
                               0,
                               graphLoaded.state == domain_cache::LoadState::stale
                                   ? graphLoaded.diagnostic
                                   : "No cached Activity Graph shard; catalogue this location to create it."};
    }

    path = {};
    activity_logic_catalog::Catalog logic;
    const bool logicPath = cache_path(key, L"\\activity-logic.bin", path, false);
    const auto logicLoaded = logicPath
                                 ? domain_cache::load_activity_logic(
                                       std::wstring_view(path.chars.data(), path.length),
                                       scope.scenarioTag,
                                       key.contentFingerprint,
                                       logic)
                                 : domain_cache::LoadResult{
                                       domain_cache::LoadState::rejected,
                                       "The Activity Logic cache path is unavailable."};
    if (logicLoaded.state == domain_cache::LoadState::ready
        && logic_provider::activate_location(std::move(logic), activation_source(scope, preview))) {
        const auto* activity = activity_logic_catalog::find_activity(
            logic_provider::state().locationCatalog, scope.scenarioTag);
        const std::size_t count = activity == nullptr ? 0 : activity->entityIndices.size();
        logicStatus = {DomainState::cached, count, logicLoaded.diagnostic};
        publicationChanged = true;
    } else {
        logicStatus =
            logicLoaded.state == domain_cache::LoadState::rejected
                ? DomainStatus{DomainState::failed, 0, logicLoaded.diagnostic}
                : DomainStatus{DomainState::idle,
                               0,
                               logicLoaded.state == domain_cache::LoadState::stale
                                   ? logicLoaded.diagnostic
                                   : "No cached Activity Logic shard; catalogue this location to create it."};
    }

    if (preview) {
        g_seenGraphRevision = graph_provider::publication_revision();
        g_seenLogicRevision = logic_provider::publication_revision();
        return publicationChanged;
    }

    path = {};
    bubble_catalog::Catalog bubbles;
    const bool bubblePath = cache_path(key, L"\\bubble-bounds.bin", path, false);
    const auto bubbleLoaded = bubblePath
                                  ? domain_cache::load_bubble_bounds(
                                        std::wstring_view(path.chars.data(), path.length),
                                        scope.mapFamily,
                                        bubbles)
                                  : domain_cache::LoadResult{
                                        domain_cache::LoadState::rejected,
                                        "The bubble-bounds cache path is unavailable."};
    if (bubbleLoaded.state == domain_cache::LoadState::ready
        && bubble_provider::activate_location(std::move(bubbles), scope.mapFamily)) {
        g_status.bubbleBounds = {DomainState::cached,
                                 bubble_provider::state().locationCatalog.bubbles.size(),
                                 bubbleLoaded.diagnostic};
        publicationChanged = true;
    } else {
        g_status.bubbleBounds =
            bubbleLoaded.state == domain_cache::LoadState::rejected
                ? DomainStatus{DomainState::failed, 0, bubbleLoaded.diagnostic}
                : DomainStatus{DomainState::idle,
                               0,
                               "No cached package-native bubble bounds; catalogue this location to create them."};
    }
    g_seenGraphRevision = graph_provider::publication_revision();
    g_seenLogicRevision = logic_provider::publication_revision();
    g_seenBubbleRevision = bubble_provider::publication_revision();
    return publicationChanged;
}

void update_inputs() noexcept {
    core::path::Buffer directoryPath{};
    g_status.canCollect = g_status.scopeAvailable && !g_collecting && package_items::readable()
                          && package_items::package_directory(directoryPath);
}

[[nodiscard]] bool consume_worker() {
    if (!g_scopeSelected || !g_collecting || statics::running()) {
        return false;
    }
    const bool preview = g_requestMode == statics::RequestMode::activityPreview;
    const Scope& requestScope = preview ? g_preview.scope : g_scope;
    const statics_cache::Key& requestKey = preview ? g_previewKey : g_key;
    DomainStatus& graphStatus =
        preview ? g_previewStatus.activityGraph : g_status.activityGraph;
    DomainStatus& logicStatus =
        preview ? g_previewStatus.activityLogic : g_status.activityLogic;
    const bool preserveCachedGraph = preview && graphStatus.state == DomainState::cached;
    const bool preserveCachedLogic = preview && logicStatus.state == DomainState::cached;
    if (!statics::publication_matches(
            requestScope.activitySession, requestScope.scenarioTag, g_requestMode)) {
        g_collecting = false;
        if (preview) {
            if (!preserveCachedGraph) {
                graphStatus = {
                    DomainState::failed, 0, "The activity preview worker did not publish."};
            }
            if (!preserveCachedLogic) {
                logicStatus = {
                    DomainState::failed, 0, "The activity preview worker did not publish."};
            }
        } else {
            g_status.statics = {
                DomainState::failed, 0, "The declared packages did not produce a valid catalogue."};
        }
        return false;
    }
    const std::uint64_t revision = statics::publication_revision();
    if (revision == g_seenStaticsRevision) {
        return false;
    }
    std::array<statics::Footprint, statics::kFootprintCapacity> rows{};
    std::size_t count = 0;
    const statics::Progress progress = statics::progress();
    g_collecting = false;
    if (!preview && !statics::snapshot(rows, count)) {
        g_status.statics = {DomainState::failed, 0, "The statics publication could not be copied."};
        return true;
    }
    g_seenStaticsRevision = revision;
    activity_catalog::Catalog graph;
    graph_packages::Progress graphProgress{};
    if (statics::graph_snapshot(graph, graphProgress)) {
        core::path::Buffer graphPath{};
        std::string graphDiagnostic;
        const bool stored = cache_path(requestKey, L"\\activity-graph.bin", graphPath, true)
                            && domain_cache::store_activity_graph_atomic(
                                std::wstring_view(graphPath.chars.data(), graphPath.length),
                                graph,
                                requestScope.scenarioTag,
                                requestKey.contentFingerprint,
                                graphDiagnostic);
        activity_catalog::Catalog checked;
        const auto loaded = stored
                                ? domain_cache::load_activity_graph(
                                      std::wstring_view(graphPath.chars.data(), graphPath.length),
                                      requestScope.scenarioTag,
                                      requestKey.contentFingerprint,
                                      checked)
                                : domain_cache::LoadResult{
                                      domain_cache::LoadState::rejected,
                                      graphDiagnostic.empty()
                                          ? "The Activity Graph cache could not be written."
                                          : graphDiagnostic};
        if (loaded.state == domain_cache::LoadState::ready
            && graph_provider::activate_location(
                std::move(checked), activation_source(requestScope, preview))) {
            graphStatus = {
                DomainState::ready, graphProgress.nodes, graphDiagnostic};
        } else if (!preserveCachedGraph) {
            graphStatus = {DomainState::failed, 0, loaded.diagnostic};
        }
    } else if (!preserveCachedGraph) {
        graph_provider::deactivate_location();
        graphStatus = {
            DomainState::unavailable,
            0,
            graphProgress.diagnostic.empty()
                ? "The current scenario exposed no fully validated Activity Graph."
                : graphProgress.diagnostic};
    }
    activity_logic_catalog::Catalog logic;
    logic_packages::Progress logicProgress{};
    if (statics::logic_snapshot(logic, logicProgress)) {
        core::path::Buffer logicPath{};
        std::string logicDiagnostic;
        const bool stored = cache_path(requestKey, L"\\activity-logic.bin", logicPath, true)
                            && domain_cache::store_activity_logic_atomic(
                                std::wstring_view(logicPath.chars.data(), logicPath.length),
                                logic,
                                requestScope.scenarioTag,
                                requestKey.contentFingerprint,
                                logicDiagnostic);
        activity_logic_catalog::Catalog checked;
        const auto loaded = stored
                                ? domain_cache::load_activity_logic(
                                      std::wstring_view(logicPath.chars.data(), logicPath.length),
                                      requestScope.scenarioTag,
                                      requestKey.contentFingerprint,
                                      checked)
                                : domain_cache::LoadResult{
                                      domain_cache::LoadState::rejected,
                                      logicDiagnostic.empty()
                                          ? "The Activity Logic cache could not be written."
                                          : logicDiagnostic};
        if (loaded.state == domain_cache::LoadState::ready
            && logic_provider::activate_location(
                std::move(checked), activation_source(requestScope, preview))) {
            logicStatus = {
                DomainState::ready, logicProgress.definitions, logicDiagnostic};
        } else if (!preserveCachedLogic) {
            logicStatus = {DomainState::failed, 0, loaded.diagnostic};
        }
    } else if (!preserveCachedLogic) {
        logic_provider::deactivate_location();
        logicStatus = {
            DomainState::unavailable,
            0,
            "The current scenario exposed no fully validated Activity Logic definitions."};
    }
    if (preview) {
        g_seenGraphRevision = graph_provider::publication_revision();
        g_seenLogicRevision = logic_provider::publication_revision();
        return true;
    }

    bubble_catalog::Catalog bubbles;
    bubble_packages::Progress bubbleProgress{};
    if (statics::bubble_snapshot(bubbles, bubbleProgress)) {
        core::path::Buffer bubblePath{};
        std::string bubbleDiagnostic;
        const bool stored = cache_path(g_key, L"\\bubble-bounds.bin", bubblePath, true)
                            && domain_cache::store_bubble_bounds_atomic(
                                std::wstring_view(bubblePath.chars.data(), bubblePath.length),
                                bubbles,
                                g_scope.mapFamily,
                                bubbleDiagnostic);
        bubble_catalog::Catalog checked;
        const auto loaded = stored
                                ? domain_cache::load_bubble_bounds(
                                      std::wstring_view(
                                          bubblePath.chars.data(), bubblePath.length),
                                      g_scope.mapFamily,
                                      checked)
                                : domain_cache::LoadResult{
                                      domain_cache::LoadState::rejected,
                                      bubbleDiagnostic.empty()
                                          ? "The package-native bubble bounds could not be cached."
                                          : bubbleDiagnostic};
        if (loaded.state == domain_cache::LoadState::ready
            && bubble_provider::activate_location(std::move(checked), g_scope.mapFamily)) {
            g_status.bubbleBounds = {
                DomainState::ready, bubbleProgress.published, bubbleDiagnostic};
        } else {
            g_status.bubbleBounds = {DomainState::failed, 0, loaded.diagnostic};
        }
    } else {
        g_status.bubbleBounds = {
            DomainState::unavailable,
            0,
            bubbleProgress.diagnostic.empty()
                ? "The current scenario exposed no fully validated package-native bubble bounds."
                : bubbleProgress.diagnostic};
    }
    core::path::Buffer path{};
    std::string diagnostic;
    const bool cached = cache_path(g_key, L"\\statics-footprints.bin", path, true)
                        && statics_cache::store_atomic(
                            std::wstring_view(path.chars.data(), path.length),
                            g_key,
                            std::span{rows.data(), count},
                            progress,
                            diagnostic);
    g_status.statics = {DomainState::ready,
                        count,
                        cached ? diagnostic
                               : diagnostic.empty() ? "Statics are ready, but the cache path failed."
                                                    : diagnostic};
    return true;
}

} // namespace

void initialize(void* module) noexcept {
    g_artifactRoot = {};
    g_initialized = core::path::artifact_directory(module, g_artifactRoot);
    g_scopeSelected = false;
    g_collecting = false;
    g_seenStaticsRevision = 0;
    g_seenGraphRevision = 0;
    g_seenLogicRevision = 0;
    g_seenBubbleRevision = 0;
    g_scope = {};
    g_key = {};
    g_preview = {};
    g_previewKey = {};
    g_previewStatus = {};
    g_status = {};
    g_requestMode = statics::RequestMode::liveLocation;
    unavailable_domains();
    statics::initialize();
}

void cancel() noexcept {
    statics::cancel();
    if (g_collecting) {
        if (g_requestMode == statics::RequestMode::activityPreview) {
            g_previewStatus.activityGraph = {DomainState::idle, 0, "Collection cancelled."};
            g_previewStatus.activityLogic = {DomainState::idle, 0, "Collection cancelled."};
        } else {
            g_status.statics = {DomainState::idle, 0, "Collection cancelled."};
        }
    }
    g_collecting = false;
    update_inputs();
}

void shutdown() noexcept {
    statics::shutdown();
    graph_provider::deactivate_location();
    logic_provider::deactivate_location();
    bubble_provider::deactivate_location();
    g_artifactRoot = {};
    g_scope = {};
    g_key = {};
    g_preview = {};
    g_previewKey = {};
    g_previewStatus = {};
    g_status = {};
    g_initialized = false;
    g_scopeSelected = false;
    g_collecting = false;
    g_requestMode = statics::RequestMode::liveLocation;
    g_seenStaticsRevision = 0;
    g_seenGraphRevision = 0;
    g_seenLogicRevision = 0;
    g_seenBubbleRevision = 0;
}

bool refresh(const Scope& scope) noexcept {
    Scope normalized{};
    statics_cache::Key key{};
    if (!g_initialized || !make_key(scope, normalized, key)) {
        const bool publicationChanged =
            g_scopeSelected
            && (statics::ready() || graph_provider::state().locationActive
                || logic_provider::state().locationActive
                || bubble_provider::state().locationActive);
        if (g_scopeSelected) {
            statics::clear();
            graph_provider::deactivate_location();
            logic_provider::deactivate_location();
            bubble_provider::deactivate_location();
            g_scopeSelected = false;
            g_collecting = false;
            g_requestMode = statics::RequestMode::liveLocation;
            g_preview = {};
            g_previewKey = {};
            g_previewStatus = {};
        }
        g_status.scopeAvailable = false;
        g_status.canCollect = false;
        unavailable_domains();
        g_status.statics = {DomainState::idle, 0, "No resolved current-location package scope."};
        return publicationChanged;
    }
    bool publicationChanged = false;
    if (!g_scopeSelected || !same_key(g_key, key)
        || g_scope.activitySession != normalized.activitySession) {
        publicationChanged = statics::ready() || graph_provider::state().locationActive
                             || logic_provider::state().locationActive
                             || bubble_provider::state().locationActive;
        statics::clear();
        graph_provider::deactivate_location();
        logic_provider::deactivate_location();
        bubble_provider::deactivate_location();
        g_scope = std::move(normalized);
        g_key = std::move(key);
        g_preview = {};
        g_previewKey = {};
        g_previewStatus = {};
        g_scopeSelected = true;
        g_collecting = false;
        g_requestMode = statics::RequestMode::liveLocation;
        g_status.scopeAvailable = true;
        unavailable_domains();
        publicationChanged = load_cache(g_scope, g_key) || publicationChanged;
        publicationChanged = load_domain_caches(g_scope, g_key) || publicationChanged;
    }
    if (statics::running() && g_requestMode == statics::RequestMode::liveLocation) {
        g_status.statics.state = DomainState::collecting;
    }
    publicationChanged = consume_worker() || publicationChanged;
    const std::uint64_t graphRevision = graph_provider::publication_revision();
    const std::uint64_t logicRevision = logic_provider::publication_revision();
    const std::uint64_t bubbleRevision = bubble_provider::publication_revision();
    publicationChanged = graphRevision != g_seenGraphRevision
                         || logicRevision != g_seenLogicRevision
                         || bubbleRevision != g_seenBubbleRevision || publicationChanged;
    g_seenGraphRevision = graphRevision;
    g_seenLogicRevision = logicRevision;
    g_seenBubbleRevision = bubbleRevision;
    update_inputs();
    return publicationChanged;
}

bool request(const Scope& scope) noexcept {
    (void)refresh(scope);
    if (!g_scopeSelected || !g_status.canCollect || g_collecting) {
        return false;
    }
    middleware::content::packages::reader::BlockKeys keys{};
    core::path::Buffer packageDirectory{};
    if (!package_items::collect_keys(keys) || !package_items::package_directory(packageDirectory)) {
        SecureZeroMemory(&keys, sizeof keys);
        g_status.statics = {DomainState::failed, 0, "Package keys or the package directory are unavailable."};
        return g_status.activityLogic.state == DomainState::ready
               || g_status.bubbleBounds.state == DomainState::ready;
    }
    if (g_previewStatus.active) {
        (void)clear_activity_preview();
    }
    // An explicit menu action is a rebuild even when this scope completed earlier.
    statics::cancel();
    statics::request_or_start(
        g_scope.activitySession,
        g_scope.scenarioTag,
        g_scope.mapFamily,
        std::wstring_view(packageDirectory.chars.data(), packageDirectory.length),
        keys,
        std::span{g_scope.packageIds.data(), g_scope.packageCount},
        g_key.contentFingerprint,
        statics::RequestMode::liveLocation);
    SecureZeroMemory(&keys, sizeof keys);
    g_collecting = statics::running();
    g_requestMode = statics::RequestMode::liveLocation;
    if (!g_collecting) {
        g_status.statics = {DomainState::failed, 0, "The statics collection worker did not start."};
        return g_status.activityLogic.state == DomainState::ready
               || g_status.bubbleBounds.state == DomainState::ready;
    }
    g_status.statics = {
        DomainState::collecting, 0, "Reading the bounded current map-package family."};
    g_status.activityLogic = {
        DomainState::collecting, 0, "Following validated Activity Logic dependencies."};
    g_status.activityGraph = {
        DomainState::collecting, 0, "Following validated Activity Graph dependencies."};
    g_status.bubbleBounds = {
        DomainState::collecting, 0, "Following validated package-native bubble dependencies."};
    update_inputs();
    return true;
}

bool select_activity(const ActivitySelection& selection) noexcept {
    Scope normalized{};
    statics_cache::Key key{};
    if (!g_initialized || !g_scopeSelected || selection.displayName.empty()
        || selection.scope.activitySession != g_scope.activitySession
        || !make_key(selection.scope, normalized, key)) {
        return false;
    }
    if (normalized.scenarioTag == g_scope.scenarioTag) {
        return clear_activity_preview();
    }
    if (g_previewStatus.active && same_key(g_previewKey, key)
        && g_preview.scope.activitySession == normalized.activitySession) {
        const auto settled = [](DomainState state) noexcept {
            return state == DomainState::cached || state == DomainState::collecting
                   || state == DomainState::ready || state == DomainState::unavailable;
        };
        if (settled(g_previewStatus.activityGraph.state)
            && settled(g_previewStatus.activityLogic.state)) {
            return true;
        }
    }

    statics::cancel();
    g_collecting = false;
    graph_provider::deactivate_location();
    logic_provider::deactivate_location();
    g_preview = selection;
    g_preview.scope = std::move(normalized);
    g_previewKey = std::move(key);
    g_previewStatus = {};
    g_previewStatus.active = true;
    g_previewStatus.displayName = g_preview.displayName;
    g_previewStatus.mapFamily = g_preview.scope.mapFamily;
    g_previewStatus.scenarioTag = g_preview.scope.scenarioTag;
    (void)load_domain_caches(g_preview.scope, g_previewKey, true);

    const bool graphCached = g_previewStatus.activityGraph.state == DomainState::cached;
    const bool logicCached = g_previewStatus.activityLogic.state == DomainState::cached;
    if (graphCached && logicCached) {
        update_inputs();
        return true;
    }

    middleware::content::packages::reader::BlockKeys keys{};
    core::path::Buffer packageDirectory{};
    if (!package_items::collect_keys(keys) || !package_items::package_directory(packageDirectory)) {
        SecureZeroMemory(&keys, sizeof keys);
        if (!graphCached) {
            g_previewStatus.activityGraph = {
                DomainState::failed, 0, "Package keys or the package directory are unavailable."};
        }
        if (!logicCached) {
            g_previewStatus.activityLogic = {
                DomainState::failed, 0, "Package keys or the package directory are unavailable."};
        }
        return graphCached || logicCached;
    }
    statics::request_or_start(
        g_preview.scope.activitySession,
        g_preview.scope.scenarioTag,
        g_preview.scope.mapFamily,
        std::wstring_view(packageDirectory.chars.data(), packageDirectory.length),
        keys,
        std::span{g_preview.scope.packageIds.data(), g_preview.scope.packageCount},
        g_previewKey.contentFingerprint,
        statics::RequestMode::activityPreview);
    SecureZeroMemory(&keys, sizeof keys);
    g_collecting = statics::running();
    g_requestMode = statics::RequestMode::activityPreview;
    if (!g_collecting) {
        if (!graphCached) {
            g_previewStatus.activityGraph = {
                DomainState::failed, 0, "The activity preview worker did not start."};
        }
        if (!logicCached) {
            g_previewStatus.activityLogic = {
                DomainState::failed, 0, "The activity preview worker did not start."};
        }
        return graphCached || logicCached;
    }
    if (!graphCached) {
        g_previewStatus.activityGraph = {
            DomainState::collecting, 0, "Following validated Activity Graph dependencies."};
    }
    if (!logicCached) {
        g_previewStatus.activityLogic = {
            DomainState::collecting, 0, "Following validated Activity Logic dependencies."};
    }
    update_inputs();
    return true;
}

bool clear_activity_preview() noexcept {
    if (!g_previewStatus.active) {
        return false;
    }
    if (g_collecting && g_requestMode == statics::RequestMode::activityPreview) {
        statics::cancel();
        g_collecting = false;
    }
    graph_provider::deactivate_location();
    logic_provider::deactivate_location();
    g_preview = {};
    g_previewKey = {};
    g_previewStatus = {};
    g_requestMode = statics::RequestMode::liveLocation;
    const bool changed = g_scopeSelected && load_domain_caches(g_scope, g_key);
    update_inputs();
    return changed || g_scopeSelected;
}

PreviewStatus preview_status() noexcept {
    return g_previewStatus;
}

Status status() noexcept {
    Status result = g_status;
    if (g_collecting) {
        result.statics.records = statics::progress().published;
    }
    return result;
}

const char* state_name(DomainState state) noexcept {
    switch (state) {
        case DomainState::idle:
            return "Idle";
        case DomainState::cached:
            return "Cached";
        case DomainState::collecting:
            return "Collecting";
        case DomainState::ready:
            return "Ready";
        case DomainState::unavailable:
            return "Unavailable";
        case DomainState::failed:
            return "Failed";
    }
    return "Unknown";
}

} // namespace sunrise::client::inspection::current_location_catalog
