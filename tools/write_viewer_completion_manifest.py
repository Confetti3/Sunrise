"""Write a deterministic manifest for the compile-verified Viewer inspection layer."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROVIDER = ROOT / "Sunrise/src/client/inspection/providers/spawn_inspection_provider.cpp"
GENERATED = ROOT / "Sunrise/src/client/inspection/providers/generated_runtime_catalog_adapters.h"
MARKDOWN = ROOT / "Sunrise/VIEWER_RUNTIME_INSPECTION.md"
JSON = ROOT / "Sunrise/viewer_runtime_inspection.json"


def present(source: str, token: str) -> bool:
    return token in source


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tier", required=True)
    args = parser.parse_args()

    source = PROVIDER.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8") if GENERATED.exists() else ""
    adapter_count = len(
        re.findall(r"const\s+(?:::)?[A-Za-z_]\w*(?:::\w+)*&\s+record\s*=", generated)
    )

    capabilities = {
        "activity_destination_scenario": present(source, "activity::snapshot_session"),
        "map_geometry_source": present(source, "[Geometry source]"),
        "region_context": present(source, "[Region context]"),
        "scenario_bubble_context": present(source, "[Scenario bubble]"),
        "map_bubble_context": present(source, "[Map bubble]"),
        "spawn_sets_and_points": present(source, "spawn_sets::snapshot_points"),
        "viewer_camera_anchor": present(source, "[Entity] Viewer camera"),
        "viewer_audio_listener": present(source, "[Audio] Viewer listener"),
        "local_player_anchor": present(source, "[Entity] Local player"),
        "movement_physics_anchor": present(source, "[Physics] Local player movement anchor"),
        "generated_public_catalog_adapters": adapter_count,
    }
    payload = {
        "schema": 1,
        "tier": args.tier,
        "capabilities": capabilities,
        "safety": {
            "hard_coded_game_offsets": False,
            "speculative_enum_members": False,
            "publish_requires_release_x64_build": True,
        },
    }
    JSON.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    rows = [
        ("Activity, destination, scenario", capabilities["activity_destination_scenario"]),
        ("Map geometry source", capabilities["map_geometry_source"]),
        ("Region context", capabilities["region_context"]),
        ("Scenario bubble context", capabilities["scenario_bubble_context"]),
        ("Map bubble context", capabilities["map_bubble_context"]),
        ("Spawn sets and points", capabilities["spawn_sets_and_points"]),
        ("Viewer camera anchor", capabilities["viewer_camera_anchor"]),
        ("Viewer audio listener", capabilities["viewer_audio_listener"]),
        ("Local player anchor", capabilities["local_player_anchor"]),
        ("Movement/physics anchor", capabilities["movement_physics_anchor"]),
    ]
    lines = [
        "# Viewer Runtime Inspection",
        "",
        f"Compile-verified completion tier: `{args.tier}`.",
        "",
        "| Adapter | Present |",
        "| --- | --- |",
    ]
    lines.extend(f"| {name} | {'yes' if value else 'no'} |" for name, value in rows)
    lines.extend(
        [
            f"| Generated public catalog adapters | {adapter_count} |",
            "",
            "The generated catalog count covers only public Sunrise count/snapshot APIs with a "
            "position-bearing record contract. The generation path rejects hard-coded game offsets, "
            "private class records, speculative enum members, and uncompiled output.",
            "",
        ]
    )
    MARKDOWN.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
