    [&]() {
    if (!key.sessionPresent) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "No committed activity session is available."});
        return;
    }
    if (key.stale) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The activity binding changed while it was sampled."});
    }
    if (!key.scenarioReady) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The scenario catalog is not ready."});
        return;
    }
    if (!key.scenarioPresent) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "No scenario layout matches the current package."});
        return;
    }
    if (key.scenarioTruncated) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The scenario bubble list is capacity-limited."});
    }
    if (key.mapStem.empty()) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The current scenario has no map-package stem."});
        return;
    }
    if (key.spawnSetHash == 0) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The current destination has no usable spawn-set hash."});
        return;
    }
    if (!key.spawnCatalogReady) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The spawn catalog is not ready."});
        return;
    }

    spawn_sets::NameHash set{};
    if (!spawn_sets::find_hash(key.mapStem, key.spawnSetHash, set)) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The current spawn-set hash is absent from this map."});
        return;
    }
    if (set.activityPackageOverflow != 0) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The spawn set's activity-package list is truncated."});
    }

    Node setNode;
    setNode.name = hex_label("Spawn set", key.spawnSetHash);
    setNode.kind = NodeKind::spawnSet;
    setNode.producer = Producer::catalog;
    setNode.provenance = Provenance::catalog;
    setNode.nativeKey = key.spawnSetHash;
    setNode.status = Status::known;
    setNode.nameHash = key.spawnSetHash;
    setNode.source = source;
    setNode.source.spawnSetHash = key.spawnSetHash;
    setNode.actions = Action::copyId;
    snapshot_.spawnSetNode = snapshot_.graph.add(std::move(setNode), graphParent);
    if (!snapshot_.spawnSetNode) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::error, "The inspection graph could not create the spawn set."});
        return;
    }

    const std::size_t pointCount = spawn_sets::point_count();
    std::vector<spawn_sets::Point> points(pointCount);
    std::size_t copied = 0;
    if (!spawn_sets::snapshot_points(points, copied) || copied != pointCount) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::error, "The spawn point bank could not be copied coherently."});
        return;
    }

    std::size_t ordinal = 0;
    for (std::size_t index = 0; index < copied; ++index) {
        const spawn_sets::Point& point = points[index];
        if (point.stemIndex != set.stemIndex || point.nameHash != key.spawnSetHash) {
            continue;
        }

        Node node;
        node.name = spawn_label(ordinal);
        node.kind = NodeKind::spawnPoint;
        node.producer = Producer::catalog;
        node.provenance = Provenance::catalog;
        node.nativeKey = (static_cast<std::uint64_t>(point.nameHash) << 32U)
                         ^ static_cast<std::uint64_t>(ordinal + 1U);
        node.status = Status::known;
        node.nameHash = point.nameHash;
        node.transform = Transform{point.position};
        node.source = source;
        node.source.spawnSetHash = key.spawnSetHash;
        node.actions = Action::focus | Action::hide | Action::isolate | Action::copyId
                       | Action::copyPosition;
        if (!snapshot_.graph.add(std::move(node), snapshot_.spawnSetNode)) {
            snapshot_.diagnostics.push_back(
                {Diagnostic::Severity::error, "The inspection graph reached its node-id capacity."});
            break;
        }
        ++ordinal;
    }

    if (ordinal == 0) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The selected spawn set contains no copied points."});
    }
    }();

    const observed::AppendResult observedResult = observed::append(
        snapshot_.graph,
        snapshot_.diagnostics,
        objects,
        triggers,
        audio,
        physics,
        source,
        graphParent);
    snapshot_.runtimeObjectGroupNode = observedResult.objectGroup;
    snapshot_.runtimeObjectNodes = observedResult.objects;
    snapshot_.triggerGroupNode = observedResult.triggerGroup;
    snapshot_.triggerNodes = observedResult.triggers;
    snapshot_.audioListenerNode = observedResult.audioListener;
    snapshot_.physicsGroupNode = observedResult.physicsGroup;
    snapshot_.physicsBodyNodes = observedResult.physicsBodies;
    const activity_graph::AppendResult activityResult =
        activity_graph::append(snapshot_.graph, snapshot_.diagnostics, source, graphParent);
    snapshot_.activityCatalogNode = activityResult.catalogNode;
    snapshot_.activityCatalogPresent = activityResult.present;
    snapshot_.activityCatalogBuildMatch = activityResult.buildMatch;
    snapshot_.activityCatalogBuild = activityResult.contentBuild;
    snapshot_.activityCatalogVersion = activityResult.version;
    snapshot_.activityCatalogDiagnostic = activityResult.diagnostic;

    const activity_logic::AppendResult logicResult =
        activity_logic::append(snapshot_.graph, snapshot_.diagnostics, source, graphParent);
    snapshot_.activityLogicNode = logicResult.root;
    snapshot_.activityLogicPresent = logicResult.present;
    snapshot_.activityLogicMatched = logicResult.matched;
    snapshot_.activityLogicActivityName = logicResult.activityName;
    snapshot_.activityLogicDestination = logicResult.destination;
    snapshot_.activityLogicDiagnostic = logicResult.diagnostic;
    snapshot_.activityLogicDefinitionCount = logicResult.definitionCount;
    snapshot_.activityLogicPlacementCount = logicResult.placementCount;
}

const WorldSnapshot& SpawnInspectionProvider::snapshot() const noexcept {
    return snapshot_;
}

void SpawnInspectionProvider::reset() noexcept {
    snapshot_ = {};
    key_ = {};
    generation_ = 0;
    ++producerEpoch_;
    if (producerEpoch_ == 0) {
        producerEpoch_ = 1;
    }
    keyPresent_ = false;
}

} // namespace sunrise::client::inspection::providers
