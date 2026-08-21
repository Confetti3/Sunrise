void SpawnInspectionProvider::rebuild(const Key& key,
                                      const live::Snapshot& livePlayer,
                                      const observed::ObjectSnapshot& objects,
                                      const observed::TriggerSnapshot& triggers,
                                      const observed::AudioSnapshot& audio,
                                      const observed::PhysicsSnapshot& physics) {
    snapshot_ = {};
    snapshot_.graph.reset(generation_);
    snapshot_.producerEpoch = producerEpoch_ == 0 ? 1 : producerEpoch_;
    snapshot_.packageName = key.packageName;
    snapshot_.mapStem = key.mapStem;
    snapshot_.activitySession = key.activitySession;
    snapshot_.activityRevision = key.activityRevision;
    snapshot_.scenarioTag = key.scenarioTag;
    snapshot_.activityLogicBrowseScenarioTag = key.activityLogicBrowseScenarioTag;
    snapshot_.spawnSetHash = key.spawnSetHash;
    snapshot_.localPlayerHandle = key.localPlayerHandle;
    snapshot_.activityIndex = key.activityIndex;
    snapshot_.region = key.region;
    snapshot_.sessionPresent = key.sessionPresent;
    snapshot_.scenarioCatalogReady = key.scenarioReady;
    snapshot_.scenarioPresent = key.scenarioPresent;
    snapshot_.scenarioTruncated = key.scenarioTruncated;
    snapshot_.localPlayerPresent = key.localPlayerPresent;
    snapshot_.localPlayerPositionPresent = key.localPlayerPositionPresent;
    snapshot_.runtimeObjectsPresent = key.runtimeObjectsPresent;
    snapshot_.runtimeObjectsTruncated = key.runtimeObjectsTruncated;
    snapshot_.runtimeObjectDeclaredCount = key.runtimeObjectDeclaredCount;
    snapshot_.runtimeObjectCount = key.runtimeObjectCount;
    snapshot_.triggersPresent = key.triggersPresent;
    snapshot_.triggersTruncated = key.triggersTruncated;
    snapshot_.triggerCount = key.triggerCount;
    snapshot_.audioListenerPresent = key.audioListenerPresent;
    snapshot_.physicsPresent = key.physicsPresent;
    snapshot_.physicsDeclaredSlots = key.physicsDeclaredSlots;
    snapshot_.physicsBodyCount = key.physicsBodyCount;
    snapshot_.physicsTruncated = key.physicsTruncated;
    snapshot_.spawnCatalogReady = key.spawnCatalogReady;
    snapshot_.stale = key.stale;
    if (key.bubble >= 0) {
        snapshot_.bubble = static_cast<std::uint16_t>(key.bubble);
    }
    if (key.mapBubble >= 0) {
        snapshot_.mapBubble = static_cast<std::uint16_t>(key.mapBubble);
    }

    Source source;
    source.packageName = key.packageName;
    source.mapStem = key.mapStem;
    if (key.scenarioTag != 0) {
        source.scenarioTag = key.scenarioTag;
    }
    if (key.spawnSetHash != 0) {
        source.spawnSetHash = key.spawnSetHash;
    }
    if (key.activitySession != 0) {
        source.activitySession = key.activitySession;
    }
    if (key.activityIndex >= 0) {
        source.activityIndex = key.activityIndex;
    }
    if (key.bubble >= 0) {
        source.bubble = static_cast<std::uint16_t>(key.bubble);
    }

    Node root;
    root.name = key.packageName.empty() ? "World" : key.packageName;
    root.kind = NodeKind::world;
    root.producer = Producer::graph;
    root.provenance = Provenance::derived;
    root.nativeKey = key.activitySession;
    root.status = key.stale ? Status::deferred
                            : (key.sessionPresent ? Status::known : Status::unknownSemantic);
    root.source = source;
    root.actions = Action::copyId;
    const NodeId rootId = snapshot_.graph.add(std::move(root));
    if (!rootId) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::error, "The inspection graph could not create its world root."});
        return;
    }

    NodeId graphParent = rootId;

    if (key.sessionPresent) {
        Node activityNode;
        activityNode.name = session_label(key.activitySession);
        activityNode.kind = NodeKind::activity;
        activityNode.producer = Producer::graph;
        activityNode.provenance = Provenance::derived;
        activityNode.nativeKey = key.activitySession;
        activityNode.status = key.stale ? Status::deferred : Status::known;
        activityNode.source = source;
        activityNode.actions = Action::copyId;
        const NodeId activityId = snapshot_.graph.add(std::move(activityNode), graphParent);
        if (activityId) {
            graphParent = activityId;
        }

        Node destinationNode;
        destinationNode.name = destination_label(key.activityIndex);
        destinationNode.kind = NodeKind::destination;
        destinationNode.producer = Producer::catalog;
        destinationNode.provenance = Provenance::catalog;
        destinationNode.nativeKey = static_cast<std::uint32_t>(key.activityIndex);
        destinationNode.status = key.stale ? Status::deferred : Status::known;
        destinationNode.source = source;
        destinationNode.actions = Action::copyId;
        const NodeId destinationId = snapshot_.graph.add(std::move(destinationNode), graphParent);
        if (destinationId) {
            graphParent = destinationId;
        }
    }

    if (key.scenarioReady || key.scenarioPresent || key.scenarioTag != 0 || !key.mapStem.empty()) {
        Node scenarioNode;
        scenarioNode.name =
            key.scenarioTag != 0 ? hex_label("Scenario", key.scenarioTag) : std::string("Scenario");
        scenarioNode.kind = NodeKind::source;
        scenarioNode.producer = Producer::catalog;
        scenarioNode.provenance = Provenance::catalog;
        scenarioNode.nativeKey = key.scenarioTag;
        scenarioNode.status = !key.scenarioReady
                                  ? Status::deferred
                                  : (key.scenarioPresent ? Status::known : Status::unknownSemantic);
        scenarioNode.source = source;
        if (key.scenarioTag != 0) {
            scenarioNode.tag = key.scenarioTag;
            scenarioNode.actions = Action::copyId | Action::copyTag;
        } else {
            scenarioNode.actions = Action::copyId;
        }
        const NodeId scenarioId = snapshot_.graph.add(std::move(scenarioNode), graphParent);
        if (scenarioId) {
            graphParent = scenarioId;
        }
    }

    const live::AppendResult liveResult =
        live::append(snapshot_.graph, snapshot_.diagnostics, livePlayer, source, graphParent);
    snapshot_.localPlayerNode = liveResult.node;

    const placed::AppendResult placedResult = placed::append(
        snapshot_.graph, snapshot_.diagnostics, key.placedObjects, source, graphParent);
    snapshot_.placedObjectGroupNode = placedResult.groupNode;
    snapshot_.placedObjectCount = placedResult.objectCount;
    snapshot_.placedObjectSlotCount = placedResult.slotCount;
    snapshot_.placedObjectSlotsTruncated = placedResult.slotsTruncated;
    add_capability_diagnostics(snapshot_.diagnostics);
    add_world_context_diagnostics(snapshot_);
