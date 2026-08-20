"""Complete evidence-backed Sunrise Viewer inspection adapters.

This script is intentionally source-driven: it reads the inspection model and public Sunrise
headers from the checked-out revision, selects only identifiers and player-position APIs that
actually exist, and then patches the Viewer provider and documentation idempotently. It never
introduces guessed enum names or hard-coded game-memory offsets.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODEL_HEADER = ROOT / "Sunrise/src/client/inspection/world_inspection_model.h"
PROVIDER_HEADER = ROOT / "Sunrise/src/client/inspection/providers/spawn_inspection_provider.h"
PROVIDER_SOURCE = ROOT / "Sunrise/src/client/inspection/providers/spawn_inspection_provider.cpp"
PLAYER_HEADER = ROOT / "Sunrise/src/client/player/player_position.h"
VIEWPORT_SOURCE = ROOT / "Sunrise/src/client/ui/world_inspector/world_inspector_viewport.cpp"
README = ROOT / "README.md"


def enum_names(text: str, enum_name: str) -> list[str]:
    match = re.search(
        rf"enum\s+class\s+{re.escape(enum_name)}\b[^{{]*\{{(.*?)\}}\s*;", text, re.S
    )
    if match is None:
        raise RuntimeError(f"{enum_name} was not found in {MODEL_HEADER}")
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.S)
    body = re.sub(r"//.*", "", body)
    result: list[str] = []
    for item in body.split(","):
        name = item.split("=", 1)[0].strip()
        if re.fullmatch(r"[A-Za-z_]\w*", name):
            result.append(name)
    return result


def choose(names: list[str], candidates: list[str], fallback: str) -> str:
    exact = {name.casefold(): name for name in names}
    for candidate in candidates:
        value = exact.get(candidate.casefold())
        if value is not None:
            return value
    for candidate in candidates:
        for name in names:
            if candidate.casefold() in name.casefold():
                return name
    return fallback


def replace_required(text: str, old: str, new: str, description: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise RuntimeError(f"could not locate {description}")
    return text.replace(old, new, 1)


def player_adapter(header: str) -> tuple[bool, str]:
    aliases = {
        match.group(1)
        for match in re.finditer(
            r"using\s+(\w+)\s*=\s*std::array\s*<\s*float\s*,\s*3\s*>\s*;", header
        )
    }
    type_expression = r"(?:std::array\s*<\s*float\s*,\s*3\s*>"
    if aliases:
        type_expression += "|" + "|".join(map(re.escape, sorted(aliases)))
    type_expression += ")"

    output_parameter = re.search(
        rf"(?:\[\[nodiscard\]\]\s*)?bool\s+(\w*position\w*)\s*\(\s*"
        rf"({type_expression})\s*&\s*\w*[^)]*\)\s*(?:noexcept)?\s*;",
        header,
        re.I | re.S,
    )
    if output_parameter is not None:
        name = output_parameter.group(1)
        return True, f"""    std::array<float, 3> position{{}};
    if (!player::{name}(position)) {{
        return false;
    }}
    out = position;
    return true;
"""

    optional_result = re.search(
        rf"(?:\[\[nodiscard\]\]\s*)?std::optional\s*<\s*({type_expression})\s*>\s+"
        rf"(\w*position\w*)\s*\(\s*\)\s*(?:noexcept)?\s*;",
        header,
        re.I | re.S,
    )
    if optional_result is not None:
        name = optional_result.group(2)
        return True, f"""    const auto position = player::{name}();
    if (!position.has_value()) {{
        return false;
    }}
    out = *position;
    return true;
"""

    direct_result = re.search(
        rf"(?:\[\[nodiscard\]\]\s*)?({type_expression})\s+(\w*position\w*)\s*"
        rf"\(\s*\)\s*(?:noexcept)?\s*;",
        header,
        re.I | re.S,
    )
    if direct_result is not None:
        name = direct_result.group(2)
        return True, f"""    out = player::{name}();
    return true;
"""

    structures = {
        match.group(1): match.group(2)
        for match in re.finditer(r"struct\s+(\w+)\s+(?:final\s*)?\{(.*?)\};", header, re.S)
    }
    for structure, body in structures.items():
        if re.search(rf"{type_expression}\s+position\b", body, re.S) is None:
            continue
        snapshot = re.search(
            rf"(?:\[\[nodiscard\]\]\s*)?bool\s+(\w*(?:snapshot|position)\w*)\s*"
            rf"\(\s*{re.escape(structure)}\s*&\s*\w*[^)]*\)\s*(?:noexcept)?\s*;",
            header,
            re.I | re.S,
        )
        if snapshot is not None:
            name = snapshot.group(1)
            return True, f"""    player::{structure} snapshot{{}};
    if (!player::{name}(snapshot)) {{
        return false;
    }}
    out = snapshot.position;
    return true;
"""

    return False, "    (void)out;\n    return false;\n"


def patch_provider() -> tuple[bool, dict[str, str]]:
    model = MODEL_HEADER.read_text(encoding="utf-8")
    kinds = enum_names(model, "NodeKind")
    source_kind = choose(kinds, ["source"], kinds[0])
    selected = {
        "entity": choose(kinds, ["entity", "object", "actor"], source_kind),
        "geometry": choose(kinds, ["geometry", "terrain", "mesh"], source_kind),
        "volume": choose(kinds, ["volume", "trigger"], source_kind),
        "audio": choose(kinds, ["audioEmitter", "audio", "sound"], source_kind),
        "physics": choose(kinds, ["physics", "rigidBody", "body"], source_kind),
    }

    header = PROVIDER_HEADER.read_text(encoding="utf-8")
    source = PROVIDER_SOURCE.read_text(encoding="utf-8")

    replacements = {
        'mapNode.kind = NodeKind::source;': f'mapNode.kind = NodeKind::{selected["geometry"]};',
        'regionNode.kind = NodeKind::source;': f'regionNode.kind = NodeKind::{selected["volume"]};',
        'bubbleNode.kind = NodeKind::source;': f'bubbleNode.kind = NodeKind::{selected["volume"]};',
        'mapBubbleNode.kind = NodeKind::source;': (
            f'mapBubbleNode.kind = NodeKind::{selected["geometry"]};'
        ),
        'cameraNode.kind = NodeKind::source;': f'cameraNode.kind = NodeKind::{selected["entity"]};',
        'audioNode.kind = NodeKind::source;': f'audioNode.kind = NodeKind::{selected["audio"]};',
    }
    for old, new in replacements.items():
        if old in source:
            source = source.replace(old, new, 1)

    supported, adapter = player_adapter(PLAYER_HEADER.read_text(encoding="utf-8"))
    if supported and "NodeId playerNode{};" not in header:
        header = header.replace(
            "    NodeId cameraNode{};\n",
            "    NodeId cameraNode{};\n    NodeId playerNode{};\n    NodeId physicsNode{};\n",
            1,
        )
        source = source.replace(
            '#include "../../hooks/viewer_camera/viewer_camera.h"\n',
            '#include "../../hooks/viewer_camera/viewer_camera.h"\n'
            '#include "../../player/player_position.h"\n',
            1,
        )
        source = source.replace(
            "namespace camera = client::viewer::camera;\n",
            "namespace camera = client::viewer::camera;\nnamespace player = client::player;\n",
            1,
        )
        helper = f"""[[nodiscard]] bool try_player_position(std::array<float, 3>& out) noexcept {{
{adapter}}}

"""
        source = source.replace(
            "void add_capability_diagnostics(std::vector<Diagnostic>& diagnostics) {\n",
            helper + "void add_capability_diagnostics(std::vector<Diagnostic>& diagnostics) {\n",
            1,
        )
        player_nodes = f"""    Node playerNode;
    playerNode.name = "[Entity] Local player";
    playerNode.kind = NodeKind::{selected["entity"]};
    playerNode.status = Status::deferred;
    playerNode.source = source;
    playerNode.actions = Action::focus | Action::hide | Action::isolate | Action::copyId
                         | Action::copyPosition;
    snapshot_.playerNode = snapshot_.graph.add(std::move(playerNode), rootId);

    Node physicsNode;
    physicsNode.name = "[Physics] Local player movement anchor";
    physicsNode.kind = NodeKind::{selected["physics"]};
    physicsNode.status = Status::deferred;
    physicsNode.source = source;
    physicsNode.actions = Action::hide | Action::isolate | Action::copyId
                          | Action::copyPosition;
    snapshot_.physicsNode = snapshot_.graph.add(std::move(physicsNode), rootId);

"""
        source = source.replace("    Node audioNode;\n", player_nodes + "    Node audioNode;\n", 1)
        runtime = """    std::array<float, 3> playerPosition{};
    const bool playerPresent = try_player_position(playerPosition);
    const std::optional<Transform> playerTransform =
        playerPresent ? std::optional<Transform>{Transform{playerPosition}} : std::nullopt;
    if (Node* node = mutable_node(snapshot_.graph, snapshot_.playerNode); node != nullptr) {
        node->status = playerPresent ? Status::known : Status::deferred;
        node->transform = playerTransform;
    }
    if (Node* node = mutable_node(snapshot_.graph, snapshot_.physicsNode); node != nullptr) {
        node->status = playerPresent ? Status::known : Status::deferred;
        node->transform = playerTransform;
    }
"""
        source = source.replace(
            "    if (Node* node = mutable_node(snapshot_.graph, snapshot_.audioListenerNode); "
            "node != nullptr) {\n"
            "        node->status = status.active ? Status::known : Status::deferred;\n"
            "        node->transform = transform;\n"
            "    }\n",
            "    if (Node* node = mutable_node(snapshot_.graph, snapshot_.audioListenerNode); "
            "node != nullptr) {\n"
            "        node->status = status.active ? Status::known : Status::deferred;\n"
            "        node->transform = transform;\n"
            "    }\n\n" + runtime,
            1,
        )
        source = source.replace(
            "The Viewer camera and Viewer audio-listener anchors are updated without rebuilding graph identity.",
            "The Viewer camera, local player, movement anchor, and Viewer audio listener update without rebuilding graph identity.",
            1,
        )

    PROVIDER_HEADER.write_text(header, encoding="utf-8")
    PROVIDER_SOURCE.write_text(source, encoding="utf-8")
    return supported, selected


def patch_viewport(selected: dict[str, str]) -> None:
    if not VIEWPORT_SOURCE.exists():
        return
    text = VIEWPORT_SOURCE.read_text(encoding="utf-8")
    if "viewer_marker_kind" in text:
        return
    condition = re.search(
        r"(?P<indent>\s*)if\s*\(\s*node\.kind\s*!=\s*model::NodeKind::spawnPoint\s*\)\s*\{",
        text,
    )
    if condition is None:
        return
    kinds = ["spawnPoint", selected["entity"], selected["audio"], selected["physics"]]
    unique: list[str] = []
    for kind in kinds:
        if kind not in unique:
            unique.append(kind)
    cases = "\n".join(f"    case model::NodeKind::{kind}:" for kind in unique)
    helper = f"""
[[nodiscard]] constexpr bool viewer_marker_kind(model::NodeKind kind) noexcept {{
    switch (kind) {{
{cases}
        return true;
    default:
        return false;
    }}
}}

"""
    namespace_anchor = re.search(r"namespace\s*\{\s*\n", text)
    if namespace_anchor is None:
        return
    text = text[: namespace_anchor.end()] + helper + text[namespace_anchor.end() :]
    text = text.replace(
        condition.group(0),
        f"{condition.group('indent')}if (!viewer_marker_kind(node.kind)) {{",
        1,
    )
    VIEWPORT_SOURCE.write_text(text, encoding="utf-8")


def patch_readme(player_supported: bool) -> None:
    text = README.read_text(encoding="utf-8")
    old = """| Activity / destination / scenario / bubble context | Supported |
| Spawn set and spawn-point inspection | Supported |
| Runtime entity enumeration | Not currently enumerated |
| Volumes / trigger semantics | Not currently enumerated |
| Physics object enumeration | Not currently enumerated |
| Geometry / terrain enumeration | Not currently enumerated |
| Light enumeration | Not currently enumerated |
| Audio emitter enumeration | Not currently enumerated |"""
    player = "Supported" if player_supported else "Deferred when no public position snapshot is available"
    new = f"""| Activity / destination / scenario / bubble context | Supported |
| Spawn set and spawn-point inspection | Supported |
| Viewer camera transform anchor | Supported; live |
| Local-player and movement/physics anchors | {player} |
| Region / scenario-bubble / map-bubble context nodes | Supported; bounds unavailable |
| Map-package geometry source nodes | Supported; mesh surfaces unavailable |
| Viewer audio-listener transform | Supported; live with Viewer camera |
| Arbitrary runtime entities and physics bodies | Not currently enumerated |
| Trigger bounds, lights, and audio emitters | Not currently enumerated |"""
    if old in text:
        text = text.replace(old, new, 1)
    README.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    before = {
        path: path.read_bytes()
        for path in (PROVIDER_HEADER, PROVIDER_SOURCE, VIEWPORT_SOURCE, README)
        if path.exists()
    }
    player_supported, selected = patch_provider()
    patch_viewport(selected)
    patch_readme(player_supported)

    changed = [path for path, content in before.items() if path.read_bytes() != content]
    print("player_position_adapter=" + ("enabled" if player_supported else "unavailable"))
    print("node_kinds=" + ",".join(f"{key}:{value}" for key, value in selected.items()))
    for path in changed:
        print("changed=" + str(path.relative_to(ROOT)))

    if args.check and changed:
        for path, content in before.items():
            path.write_bytes(content)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
