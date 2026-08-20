
bool SpawnInspectionProvider::refresh() {
    Key next{};
    const live::Snapshot livePlayer = live::capture();
    next.localPlayerPresent = livePlayer.handlePresent;
    next.localPlayerPositionPresent = livePlayer.positionPresent;
    if (livePlayer.handlePresent) {
        next.localPlayerHandle = livePlayer.controlledHandle;
    }
    activity::SessionSnapshot session{};
    server::bap::ActivitySnapshot privateActivity{};
    const bool privatePresent = server::bap::snapshot_private_activity(privateActivity);
    if (privatePresent) {
        next.sessionPresent = activity::snapshot_session(privateActivity.binding.sessionId, session);
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
        snapshot_.localPlayerPositionPresent = livePlayer.positionPresent;
        return false;
    }
    key_ = next;
    keyPresent_ = true;
    if (generation_ == (std::numeric_limits<std::uint32_t>::max)()) {
        generation_ = 1;
    } else {
        ++generation_;
        if (generation_ == 0) {
            generation_ = 1;
        }
    }
    rebuild(next, livePlayer);
    return true;
}

