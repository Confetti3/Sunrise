# Viewer useful-data native producer audit (2026-08-20)

Target: Destiny 2 build 86657, image SHA-256
`36A04F76B8A7D0778EB2A45CF05688A26817175C67D7DD43375743CA016B65A4`.

Reference inputs were verified before migration:

- PE SHA-256: `FBAAE84129D1080157225D2E8D12A59E1D389DBA4C3736800A3CF790B36C833D`
- PDB SHA-256: `16713656EE7F336EBD1B5866A23EA3D41F07815082DDB8F206E3AEF16B57B4B6`
- PE/PDB RSDS: `{17D16314-31E5-41E1-AB9A-909B0BC13F7D}`, age 1

The PDB is an analysis-generated public-symbol map without the private layouts needed to establish
an ABI. Migrated names and RVAs below are research anchors, not production offsets.

## Entity findings

- `world_entity_array_foreach`: `0xD28FE0 -> 0xD2AC60`, unique normalized match. It requires an
  unqualified world owner and traverses state at owner `+0xC60`; no current Viewer boundary proves
  that owner's thread or lifetime.
- `world_entity_find_by_handle`: `0x12F7AA0 -> 0x12FB940`, unique normalized match. The function
  returns a boolean comparison result; it is not a safe entity-pointer accessor.
- `world_entity_get_position_quat`: `0xBC0110 -> 0xBC2250`, unique normalized match. It consumes an
  entity pointer and copies one 16-byte value. It does not prove the requested complete
  position/quaternion ABI, ownership, or lifetime.
- `world_entity_geometry_bounds_update`: `0xACE470 -> 0xAD0BB0`, unique normalized match, but it is
  a mutating update traversal rather than a read-only bounds accessor.
- `world_entity_transform_query`: no unique 16-instruction match.

Therefore complete quaternion, parent/owner handles, world identifier, and bounds remain
explicitly unavailable. The existing occupied-datum iterator and physics-owned copy boundary stay
the only enabled entity producer.

## Independently gated producers

- Audio (`audio_emitter_slot_alloc 0xD86930`, `audio_emitter_params_init 0x1990590`): allocation and
  initialization do not enumerate active emitters or prove destruction and transform ownership.
- Navigation (`navmesh_find_nearest_point 0xFDC010`): a caller-owned query, not stable navmesh
  enumeration; world-thread queueing and layouts are unproven.
- Lighting (`lighting_build_light_list_buffer 0x114F420`, `gfx_update_light_params 0x11CF9B0`):
  candidate transient frame boundaries without stable identity or copied record layout.
- Terrain (`terrain_bounds_compute_planes 0xF2280`, `terrain_tile_lookup_and_prefetch 0x1A726E0`):
  neither boundary enumerates live tiles or proves residency and identity.
- Triggers (`trigger_volume_contains_point 0x107DD90`): the existing copied event/Havok callback
  producer remains safer; shape ownership and bounds are unproven.

These producers are reported as unavailable in inspection exports. Failure to install or prove one
does not affect Viewer startup or the existing inspection graph.
