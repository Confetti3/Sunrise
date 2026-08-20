"""Generate conservative World Inspector adapters from Sunrise public catalog APIs.

The generator recognizes only a narrow, compileable contract already used by Sunrise catalogs:

* a public record struct with a three/four-component position-like field;
* a zero-argument count/size function; and
* a bool snapshot/copy function accepting ``std::span<Record>`` and ``std::size_t&``.

It never emits memory offsets, guesses undocumented field layouts, or calls private implementation
symbols. Unsupported headers are skipped. The generated header is deterministic and is compiled by
the normal Release x64 build before it may be committed.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT / "Sunrise/src"
MODEL_HEADER = SOURCE_ROOT / "client/inspection/world_inspection_model.h"
PROVIDER_SOURCE = SOURCE_ROOT / "client/inspection/providers/spawn_inspection_provider.cpp"
GENERATED_HEADER = SOURCE_ROOT / "client/inspection/providers/generated_runtime_catalog_adapters.h"
README = ROOT / "README.md"

CATEGORY_WORDS: dict[str, tuple[str, ...]] = {
    "light": ("light", "lighting"),
    "audio": ("audio", "sound", "emitter", "wwise"),
    "physics": ("physics", "rigid", "body", "havok", "collision"),
    "volume": ("trigger", "volume", "region", "bubble"),
    "entity": ("entity", "object", "actor", "character", "pawn"),
    "geometry": ("geometry", "terrain", "mesh", "scene", "static", "map"),
}

EXCLUDED_PARTS = {
    "inspection",
    "third_party",
    "thirdparty",
    "external",
    "vendor",
    "imgui",
    "detours",
    "test",
    "tests",
}


@dataclass(frozen=True)
class Record:
    header: Path
    namespace: str
    structure: str
    position: str
    position_lanes: int
    count_function: str
    snapshot_function: str
    category: str
    name_hash: str | None
    tag: str | None

    @property
    def qualified_structure(self) -> str:
        return f"{self.namespace}::{self.structure}" if self.namespace else self.structure

    @property
    def qualified_count(self) -> str:
        return f"{self.namespace}::{self.count_function}" if self.namespace else self.count_function

    @property
    def qualified_snapshot(self) -> str:
        return (
            f"{self.namespace}::{self.snapshot_function}"
            if self.namespace
            else self.snapshot_function
        )


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


def choose_kind(names: list[str], candidates: tuple[str, ...], fallback: str) -> str:
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


def namespace_at(text: str, offset: int) -> str:
    """Return the innermost simple namespace chain that is open at *offset*.

    Sunrise headers primarily use ``namespace a::b {``. A small brace scanner avoids accidentally
    associating declarations after a closed namespace with the earlier namespace.
    """

    token = re.compile(r"namespace\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\{|[{}]")
    stack: list[tuple[str, int]] = []
    depth = 0
    for match in token.finditer(text, 0, offset):
        value = match.group(0)
        if value.startswith("namespace"):
            name = match.group(1)
            stack.append((name, depth))
            depth += 1
        elif value == "{":
            depth += 1
        else:
            depth = max(0, depth - 1)
            while stack and stack[-1][1] >= depth:
                stack.pop()
    return "::".join(name for name, _ in stack)


def canonical_stem(name: str) -> str:
    value = re.sub(r"^(?:snapshot|copy|enumerate|read|get)_?", "", name, flags=re.I)
    value = re.sub(r"_?(?:count|size)$", "", value, flags=re.I)
    value = value.casefold().replace("_", "")
    if value.endswith("ies"):
        value = value[:-3] + "y"
    elif value.endswith("s") and not value.endswith("ss"):
        value = value[:-1]
    return value


def classify(path: Path, namespace: str, structure: str, snapshot: str) -> str | None:
    text = " ".join((path.as_posix(), namespace, structure, snapshot)).casefold()
    if "spawn" in text:
        return None
    for category, words in CATEGORY_WORDS.items():
        if any(re.search(rf"(?:^|[^a-z]){re.escape(word)}(?:[^a-z]|$)", text) for word in words):
            return category
    return None


def field_name(body: str, candidates: tuple[str, ...], type_expression: str) -> str | None:
    for candidate in candidates:
        match = re.search(
            rf"(?:{type_expression})\s+({re.escape(candidate)})\b", body, re.I | re.S
        )
        if match is not None:
            return match.group(1)
    return None


def discover_header(path: Path) -> list[Record]:
    relative = path.relative_to(ROOT)
    lowered_parts = {part.casefold() for part in relative.parts}
    if lowered_parts & EXCLUDED_PARTS:
        return []

    text = path.read_text(encoding="utf-8", errors="ignore")
    records: list[Record] = []
    structs = list(
        re.finditer(
            r"(?:struct|class)\s+([A-Za-z_]\w*)\s*(?:final\s*)?\{(.*?)\}\s*;",
            text,
            re.S,
        )
    )
    if not structs:
        return records

    count_matches = list(
        re.finditer(
            r"(?:\[\[nodiscard\]\]\s*)?(?:inline\s+)?std::size_t\s+"
            r"([A-Za-z_]\w*(?:count|size))\s*\(\s*\)\s*(?:noexcept)?\s*;",
            text,
            re.I | re.S,
        )
    )
    snapshot_matches = list(
        re.finditer(
            r"(?:\[\[nodiscard\]\]\s*)?(?:inline\s+)?bool\s+"
            r"([A-Za-z_]\w*(?:snapshot|copy|enumerate|read)[A-Za-z_0-9]*|"
            r"(?:snapshot|copy|enumerate|read)[A-Za-z_0-9]*)\s*\(\s*"
            r"std::span\s*<\s*(?:const\s+)?([A-Za-z_]\w*)\s*>\s+"
            r"[A-Za-z_]\w*\s*,\s*std::size_t\s*&\s*[A-Za-z_]\w*\s*\)\s*"
            r"(?:noexcept)?\s*;",
            text,
            re.I | re.S,
        )
    )
    if not count_matches or not snapshot_matches:
        return records

    for struct_match in structs:
        structure = struct_match.group(1)
        body = struct_match.group(2)
        position_match = re.search(
            r"std::array\s*<\s*float\s*,\s*([34])\s*>\s+"
            r"(position|translation|center|origin|location)\b",
            body,
            re.I | re.S,
        )
        if position_match is None:
            position_match = re.search(
                r"float\s+(position|translation|center|origin|location)\s*\[\s*([34])\s*\]",
                body,
                re.I | re.S,
            )
            if position_match is None:
                continue
            position = position_match.group(1)
            lanes = int(position_match.group(2))
        else:
            lanes = int(position_match.group(1))
            position = position_match.group(2)

        namespace = namespace_at(text, struct_match.start())
        snapshots = [match for match in snapshot_matches if match.group(2) == structure]
        for snapshot_match in snapshots:
            snapshot_name = snapshot_match.group(1)
            snapshot_stem = canonical_stem(snapshot_name)
            ranked = sorted(
                count_matches,
                key=lambda item: (
                    canonical_stem(item.group(1)) != snapshot_stem,
                    item.start() > snapshot_match.start(),
                    abs(item.start() - snapshot_match.start()),
                ),
            )
            if not ranked:
                continue
            count_match = ranked[0]
            count_name = count_match.group(1)
            if canonical_stem(count_name) != snapshot_stem and not (
                len(count_matches) == 1 and len(snapshots) == 1
            ):
                continue

            category = classify(relative, namespace, structure, snapshot_name)
            if category is None:
                continue

            integer_type = r"(?:std::)?u?int(?:32|64)_t|unsigned(?:\s+long\s+long|\s+long|\s+int)?"
            name_hash = field_name(body, ("nameHash", "name_hash", "hash"), integer_type)
            tag = field_name(body, ("tag", "tagId", "tag_id"), integer_type)
            records.append(
                Record(
                    header=relative,
                    namespace=namespace,
                    structure=structure,
                    position=position,
                    position_lanes=lanes,
                    count_function=count_name,
                    snapshot_function=snapshot_name,
                    category=category,
                    name_hash=name_hash,
                    tag=tag,
                )
            )
    return records


def humanize(value: str) -> str:
    words = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", value).replace("_", " ")
    return " ".join(part for part in words.split() if part).strip() or "Runtime record"


def unique_records(records: list[Record]) -> list[Record]:
    seen: set[tuple[str, str, str]] = set()
    result: list[Record] = []
    for record in sorted(
        records,
        key=lambda item: (
            item.category,
            item.header.as_posix(),
            item.namespace,
            item.structure,
            item.snapshot_function,
        ),
    ):
        key = (record.namespace, record.structure, record.snapshot_function)
        if key in seen:
            continue
        seen.add(key)
        result.append(record)
    return result[:32]


def render_header(records: list[Record], kinds: dict[str, str]) -> str:
    includes = sorted({record.header.as_posix() for record in records})
    lines = [
        "#pragma once",
        "",
        "// Generated by tools/generate_viewer_catalog_adapters.py. Do not hand-edit.",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdio>",
        "#include <string>",
        "#include <vector>",
        "",
    ]
    provider_dir = GENERATED_HEADER.parent
    for include in includes:
        relative = Path(include).relative_to("Sunrise/src")
        target = SOURCE_ROOT / relative
        relative_include = Path(
            *Path(provider_dir.relative_to(SOURCE_ROOT)).parts
        )
        # pathlib.relative_to cannot express upward traversal; use os.path.relpath without
        # introducing platform separators into generated C++ includes.
        import os

        path = os.path.relpath(target, provider_dir).replace("\\", "/")
        lines.append(f'#include "{path}"')
    if includes:
        lines.append("")

    lines.extend(
        [
            "namespace sunrise::client::inspection::providers::generated {",
            "",
            "inline void append_runtime_catalogs(Graph& graph,",
            "                                    NodeId parent,",
            "                                    const Source& source,",
            "                                    std::vector<Diagnostic>& diagnostics) {",
        ]
    )
    if not records:
        lines.extend(
            [
                "    (void)graph;",
                "    (void)parent;",
                "    (void)source;",
                "    (void)diagnostics;",
            ]
        )
    for index, record in enumerate(records):
        label = humanize(record.structure).replace('"', "'")
        lines.extend(
            [
                "    {",
                f"        const std::size_t count = {record.qualified_count}();",
                f"        std::vector<{record.qualified_structure}> records(count);",
                "        std::size_t copied = 0;",
                f"        if (!{record.qualified_snapshot}(records, copied) || copied > records.size()) {{",
                "            diagnostics.push_back(",
                "                {Diagnostic::Severity::warning,",
                f'                 "Generated {record.category} catalog adapter could not copy {label} records coherently."}});',
                "        } else {",
                "            for (std::size_t ordinal = 0; ordinal < copied; ++ordinal) {",
                f"                const {record.qualified_structure}& record = records[ordinal];",
                "                std::array<char, 96> labelText{};",
                "                const int written = std::snprintf(labelText.data(),",
                "                                                  labelText.size(),",
                f'                                                  "[{record.category.capitalize()}] {label} %04zu",',
                "                                                  ordinal + 1);",
                "                Node node;",
                "                node.name = written > 0",
                "                                && static_cast<std::size_t>(written) < labelText.size()",
                "                            ? std::string(labelText.data(),",
                "                                          static_cast<std::size_t>(written))",
                f'                            : std::string("[{record.category.capitalize()}] {label}");',
                f"                node.kind = NodeKind::{kinds[record.category]};",
                "                node.status = Status::known;",
                "                node.transform = Transform{{{",
                f"                    record.{record.position}[0],",
                f"                    record.{record.position}[1],",
                f"                    record.{record.position}[2],",
                "                }}};",
                "                node.source = source;",
                "                node.actions = Action::focus | Action::hide | Action::isolate",
                "                               | Action::copyId | Action::copyPosition;",
            ]
        )
        if record.name_hash:
            lines.extend(
                [
                    f"                node.nameHash = static_cast<std::uint32_t>(record.{record.name_hash});",
                ]
            )
        if record.tag:
            lines.extend(
                [
                    f"                node.tag = static_cast<std::uint32_t>(record.{record.tag});",
                    "                node.actions = node.actions | Action::copyTag;",
                ]
            )
        lines.extend(
            [
                "                if (!graph.add(std::move(node), parent)) {",
                "                    diagnostics.push_back(",
                "                        {Diagnostic::Severity::error,",
                f'                         "Generated {record.category} catalog adapter reached graph capacity."}});',
                "                    break;",
                "                }",
                "            }",
                "        }",
                "    }",
            ]
        )
    lines.extend(["}", "", "} // namespace sunrise::client::inspection::providers::generated", ""])
    return "\n".join(lines)


def patch_provider() -> None:
    text = PROVIDER_SOURCE.read_text(encoding="utf-8")
    include = '#include "generated_runtime_catalog_adapters.h"\n'
    if include not in text:
        text = text.replace(
            '#include "spawn_inspection_provider.h"\n',
            '#include "spawn_inspection_provider.h"\n\n' + include,
            1,
        )

    call = "    generated::append_runtime_catalogs(\n        snapshot_.graph, contextParent, source, snapshot_.diagnostics);\n"
    if "generated::append_runtime_catalogs" not in text:
        anchors = (
            "    Node cameraNode;\n",
            "    add_capability_diagnostics(snapshot_.diagnostics);\n",
        )
        for anchor in anchors:
            if anchor in text:
                text = text.replace(anchor, call + "\n" + anchor, 1)
                break
        else:
            raise RuntimeError("could not locate provider runtime-catalog insertion point")
    PROVIDER_SOURCE.write_text(text, encoding="utf-8")


def patch_readme(records: list[Record]) -> None:
    text = README.read_text(encoding="utf-8")
    marker = "<!-- viewer-generated-catalogs -->"
    if records:
        counts: dict[str, int] = {}
        for record in records:
            counts[record.category] = counts.get(record.category, 0) + 1
        detail = ", ".join(f"{category}={counts[category]}" for category in sorted(counts))
        paragraph = (
            f"{marker}\nThe build-generated inspection pass currently discovered {len(records)} "
            f"position-bearing public catalog adapter(s) ({detail}). These adapters are generated "
            "from checked-in Sunrise headers and compile-tested before publication.\n"
        )
    else:
        paragraph = (
            f"{marker}\nNo additional position-bearing public catalog API currently matches the "
            "conservative generated-adapter contract; live Viewer, player, world-context, and spawn "
            "adapters remain available.\n"
        )
    if marker in text:
        text = re.sub(rf"{re.escape(marker)}\n.*?(?=\n\n|\Z)", paragraph.rstrip(), text, count=1, flags=re.S)
    else:
        anchor = "Unknown or\nunsupported runtime semantics are left unknown rather than assigned speculative names.\n"
        if anchor in text:
            text = text.replace(anchor, anchor + "\n" + paragraph, 1)
        else:
            text += "\n" + paragraph
    README.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    candidates: list[Record] = []
    for header in SOURCE_ROOT.rglob("*.h"):
        candidates.extend(discover_header(header))
    records = unique_records(candidates)

    node_kinds = enum_names(MODEL_HEADER.read_text(encoding="utf-8"), "NodeKind")
    fallback = choose_kind(node_kinds, ("source",), node_kinds[0])
    kinds = {
        category: choose_kind(node_kinds, words, fallback)
        for category, words in CATEGORY_WORDS.items()
    }

    generated = render_header(records, kinds)
    before: dict[Path, bytes | None] = {
        GENERATED_HEADER: GENERATED_HEADER.read_bytes() if GENERATED_HEADER.exists() else None,
        PROVIDER_SOURCE: PROVIDER_SOURCE.read_bytes(),
        README: README.read_bytes(),
    }
    GENERATED_HEADER.write_text(generated, encoding="utf-8")
    patch_provider()
    patch_readme(records)

    changed = [
        path
        for path, content in before.items()
        if content is None or path.read_bytes() != content
    ]
    print(f"generated_catalog_count={len(records)}")
    for record in records:
        print(
            "adapter="
            + ":".join(
                (
                    record.category,
                    record.header.as_posix(),
                    record.namespace,
                    record.structure,
                    record.snapshot_function,
                )
            )
        )
    for path in changed:
        print("changed=" + str(path.relative_to(ROOT)))

    if args.check and changed:
        for path, content in before.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(content)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
