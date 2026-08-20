"""Remove only optional generated Viewer layers when a compile-gated adapter fails."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROVIDER_HEADER = ROOT / "Sunrise/src/client/inspection/providers/spawn_inspection_provider.h"
PROVIDER_SOURCE = ROOT / "Sunrise/src/client/inspection/providers/spawn_inspection_provider.cpp"
VIEWPORT_SOURCE = ROOT / "Sunrise/src/client/ui/world_inspector/world_inspector_viewport.cpp"
GENERATED_HEADER = (
    ROOT / "Sunrise/src/client/inspection/providers/generated_runtime_catalog_adapters.h"
)
README = ROOT / "README.md"


def remove_catalog_layer() -> None:
    source = PROVIDER_SOURCE.read_text(encoding="utf-8")
    source = source.replace('#include "generated_runtime_catalog_adapters.h"\n\n', "")
    source = source.replace('#include "generated_runtime_catalog_adapters.h"\n', "")
    source = re.sub(
        r"\n?\s*generated::append_runtime_catalogs\(\s*"
        r"snapshot_\.graph\s*,\s*contextParent\s*,\s*source\s*,\s*"
        r"snapshot_\.diagnostics\s*\);\s*\n",
        "\n",
        source,
        count=1,
        flags=re.S,
    )
    PROVIDER_SOURCE.write_text(source, encoding="utf-8")
    GENERATED_HEADER.unlink(missing_ok=True)

    readme = README.read_text(encoding="utf-8")
    readme = re.sub(
        r"\n?<!-- viewer-generated-catalogs -->\n.*?(?=\n\n|\Z)",
        "",
        readme,
        count=1,
        flags=re.S,
    )
    README.write_text(readme, encoding="utf-8")


def remove_dynamic_player_layer() -> None:
    header = PROVIDER_HEADER.read_text(encoding="utf-8")
    header = header.replace("    NodeId playerNode{};\n", "")
    header = header.replace("    NodeId physicsNode{};\n", "")
    PROVIDER_HEADER.write_text(header, encoding="utf-8")

    source = PROVIDER_SOURCE.read_text(encoding="utf-8")
    source = source.replace('#include "../../player/player_position.h"\n', "")
    source = source.replace("namespace player = client::player;\n", "")
    source = re.sub(
        r"\n?\[\[nodiscard\]\]\s+bool\s+try_player_position\s*\(.*?\n\}\n\n"
        r"(?=void add_capability_diagnostics)",
        "\n",
        source,
        count=1,
        flags=re.S,
    )
    source = re.sub(
        r"\n\s*Node playerNode;.*?\n\s*Node audioNode;",
        "\n\n    Node audioNode;",
        source,
        count=1,
        flags=re.S,
    )
    source = re.sub(
        r"\n\s*std::array<float, 3> playerPosition\{\};.*?"
        r"if \(Node\* node = mutable_node\(snapshot_\.graph, snapshot_\.physicsNode\);"
        r" node != nullptr\) \{.*?\n\s*\}\n",
        "\n",
        source,
        count=1,
        flags=re.S,
    )
    source = source.replace(
        "The Viewer camera, local player, movement anchor, and Viewer audio listener update "
        "without rebuilding graph identity.",
        "The Viewer camera and Viewer audio-listener anchors are updated without rebuilding "
        "graph identity.",
    )
    PROVIDER_SOURCE.write_text(source, encoding="utf-8")

    viewport = VIEWPORT_SOURCE.read_text(encoding="utf-8")
    helper_start = viewport.find("[[nodiscard]] constexpr bool viewer_marker_kind(")
    if helper_start >= 0:
        helper_end = viewport.find("\n}\n", helper_start)
        if helper_end < 0:
            raise RuntimeError("generated viewport marker helper is incomplete")
        helper_end += len("\n}\n")
        viewport = viewport[:helper_start] + viewport[helper_end:]
    viewport = viewport.replace(
        "if (!viewer_marker_kind(node.kind)) {",
        "if (node.kind != model::NodeKind::spawnPoint) {",
    )
    VIEWPORT_SOURCE.write_text(viewport, encoding="utf-8")

    readme = README.read_text(encoding="utf-8")
    readme = readme.replace(
        "| Local-player and movement/physics anchors | Supported |",
        "| Local-player and movement/physics anchors | Deferred when no compatible public position snapshot is available |",
    )
    README.write_text(readme, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalogs", action="store_true")
    parser.add_argument("--dynamic-player", action="store_true")
    args = parser.parse_args()
    if not args.catalogs and not args.dynamic_player:
        parser.error("select at least one optional layer to remove")
    if args.catalogs:
        remove_catalog_layer()
    if args.dynamic_player:
        remove_dynamic_player_layer()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
