
bool SpawnInspectionProvider::refresh() {
    Key next{};
    const live::Snapshot livePlayer = live::capture();
    const observed::ObjectSnapshot objects = observed::capture_objects();
    const observed::TriggerSnapshot triggers = observed::capture_triggers();
    const observed::AudioSnapshot audio = observed::capture_audio();
    const observed::PhysicsSnapshot physics = observed::capture_physics();
    next.localPlayerPresent = livePlayer.handlePresent;
    next.localPlayerPositionPresent = livePlayer.positionPresent;
    next.runtimeObjectsPresent = objects.present;
    next.runtimeObjectsTruncated = objects.truncated;
    next.runtimeObjectDeclaredCount = objects.declaredCount;
    next.runtimeObjectCount = objects.objectCount;
    for (std::size_t index = 0; index < objects.objectCount; ++index) {
        next.runtimeObjectHandles[index] = objects.objects[index].handle;
        next.runtimeObjectTypes[index] = objects.objects[index].type;
    }
    next.triggersPresent = triggers.present;
    next.triggersTruncated = triggers.truncated;
    next.triggerCount = triggers.triggerCount;
    for (std::size_t index = 0; index < triggers.triggerCount; ++index) {
        next.triggerIdentities[index] = observed::trigger_identity(triggers.triggers[index]);
    }
    if (livePlayer.handlePresent) {
        next.localPlayerHandle = livePlayer.controlledHandle;
    }
    next.audioListenerPresent = audio.present;
    next.physicsPresent = physics.present;
    next.physicsDeclaredSlots = physics.declaredSlots;
    next.physicsBodyCount = physics.bodyCount;
    next.physicsTruncated = physics.truncated;
    for (std::size_t index = 0; index < physics.bodyCount; ++index) {
        next.physicsSlots[index] = physics.bodies[index].slot;
    }
    activity::SessionSnapshot session{};
    server::bap::ActivitySnapshot privateActivity{};
    const bool privatePresent = server::bap::snapshot_private_activity(privateActivity);
    if (privatePresent) {
        next.sessionPresent =
            activity::snapshot_session(privateActivity.binding.sessionId, session);
        next.stale = !next.sessionPresent
                     || session.binding.sessionId != privateActivity.binding.sessionId
                     || session.binding.createdRevision != privateActivity.binding.createdRevision;
    } else {
        const std::uint64_t regionSession =
            activity::membership::live_region_session(activity::kAbsentSessionId);
        next.sessionPresent = regionSession != activity::kAbsentSessionId
                              && activity::snapshot_session(regionSession, session);
        next.stale = regionSession != activity::kAbsentSessionId && !next.sessionPresent;
    }

    if (next.sessionPresent) {
        next.activitySession = session.binding.sessionId;
        next.activityRevision = session.binding.createdRevision;
        next.activityIndex = session.binding.destination.activityIndex;
        next.region = session.reportedRegion;
        next.packageName = package_name(session.binding.destination);
        next.scenarioReady = state::build_data::scenario_layouts_ready();

        layouts::Definition layout{};
        next.scenarioPresent = next.scenarioReady && layouts::find(next.packageName, layout);
        if (next.scenarioPresent) {
            next.scenarioTag = layout.tag;
            next.mapStem = stem_name(layout);
            next.scenarioTruncated = layout.truncated != 0;
            if (next.region >= 0) {
                const std::size_t bubble =
                    static_cast<std::size_t>(next.region) / tables::kSliceSetIndexFactor;
                if (bubble < layout.bubbleCount) {
                    next.bubble = static_cast<std::int32_t>(bubble);
                    next.mapBubble = static_cast<std::int32_t>(layout.bubbleMapIndices[bubble]);
                }
            }
            placed::collect(layout, next.bubble, next.placedObjects);
        }

        const auto& destination = session.binding.destination;
        if (destination.hasSpawnSetOverride) {
            next.spawnSetHash = destination.spawnSetOverride;
        } else if (activity::destination::usable_spawn_set_hash(destination.hasSpawnSetHash,
                                                                destination.spawnSetHash)) {
            next.spawnSetHash = destination.spawnSetHash;
        }
        next.spawnCatalogReady = state::build_data::spawn_sets_ready();
        next.stale = next.stale || !activity::binding_matches(session.binding);
    }

    if (keyPresent_ && next == key_) {
        live::update(snapshot_.graph, snapshot_.localPlayerNode, livePlayer);
        observed::update_objects(snapshot_.graph, snapshot_.runtimeObjectNodes, objects);
        observed::update_triggers(snapshot_.graph, snapshot_.triggerNodes, triggers, objects);
        observed::update_audio(snapshot_.graph, snapshot_.audioListenerNode, audio);
        observed::update_physics(snapshot_.graph, snapshot_.physicsBodyNodes, physics);
        snapshot_.localPlayerPositionPresent = livePlayer.positionPresent;
        return false;
    }
    Key structuralNext = next;
    Key structuralCurrent = key_;
    const auto clearRuntimeObservations = [](Key& key) noexcept {
        key.physicsSlots = {};
        key.runtimeObjectHandles = {};
        key.runtimeObjectTypes = {};
        key.triggerIdentities = {};
        key.physicsDeclaredSlots = 0;
        key.physicsBodyCount = 0;
        key.runtimeObjectDeclaredCount = 0;
        key.runtimeObjectCount = 0;
        key.triggerCount = 0;
        key.audioListenerPresent = false;
        key.physicsPresent = false;
        key.physicsTruncated = false;
        key.runtimeObjectsPresent = false;
        key.runtimeObjectsTruncated = false;
        key.triggersPresent = false;
        key.triggersTruncated = false;
    };
    clearRuntimeObservations(structuralNext);
    clearRuntimeObservations(structuralCurrent);
    const bool producerReset = !keyPresent_ || next.activitySession != key_.activitySession
                               || next.activityRevision != key_.activityRevision;
    const bool structuralChanged = !keyPresent_ || structuralNext != structuralCurrent;
    if (producerReset) {
        client::viewer::objects::reset_activity();
        ++producerEpoch_;
        if (producerEpoch_ == 0) {
            producerEpoch_ = 1;
        }
    }
    key_ = next;
    keyPresent_ = true;
    if (structuralChanged) {
        if (generation_ == (std::numeric_limits<std::uint32_t>::max)()) {
            generation_ = 1;
        } else {
            ++generation_;
            if (generation_ == 0) {
                generation_ = 1;
            }
        }
    }
    rebuild(next, livePlayer, objects, triggers, audio, physics);
    return true;
}
