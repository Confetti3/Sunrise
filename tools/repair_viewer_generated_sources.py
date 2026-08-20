"""Repair source-order and include details after source-driven Viewer generation."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VIEWPORT = ROOT / "Sunrise/src/client/ui/world_inspector/world_inspector_viewport.cpp"


def repair_viewport(text: str) -> str:
    marker = "viewer_marker_kind"
    alias = "namespace model = client::inspection;"
    if marker not in text or alias not in text:
        return text

    helper_start = text.find("[[nodiscard]] constexpr bool viewer_marker_kind(")
    alias_offset = text.find(alias)
    if helper_start < 0 or helper_start > alias_offset:
        return text

    helper_end = text.find("\n}\n", helper_start)
    if helper_end < 0:
        raise RuntimeError("generated viewport marker helper is incomplete")
    helper_end += len("\n}\n")
    helper = text[helper_start:helper_end]
    helper = helper.replace(
        "model::NodeKind", "::sunrise::client::inspection::NodeKind"
    )
    return text[:helper_start] + helper + text[helper_end:]


def main() -> int:
    if VIEWPORT.exists():
        original = VIEWPORT.read_text(encoding="utf-8")
        updated = repair_viewport(original)
        VIEWPORT.write_text(updated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
