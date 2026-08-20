"""Apply conservative parser hardening to the Viewer catalog generator idempotently."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools/generate_viewer_catalog_adapters.py"


def replace_once(text: str, old: str, new: str, description: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise RuntimeError(f"could not harden {description}")
    return text.replace(old, new, 1)


def main() -> int:
    text = GENERATOR.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '            r"(?:struct|class)\\s+([A-Za-z_]\\w*)\\s*(?:final\\s*)?\\{(.*?)\\}\\s*;",',
        '            r"struct\\s+([A-Za-z_]\\w*)\\s*(?:final\\s*)?\\{(.*?)\\}\\s*;",',
        "record visibility",
    )

    text = replace_once(
        text,
        '        return f"{self.namespace}::{self.structure}" if self.namespace else self.structure',
        '        return f"::{self.namespace}::{self.structure}" if self.namespace else self.structure',
        "qualified record type",
    )
    text = replace_once(
        text,
        '        return f"{self.namespace}::{self.count_function}" if self.namespace else self.count_function',
        '        return f"::{self.namespace}::{self.count_function}" if self.namespace else self.count_function',
        "qualified count function",
    )
    text = replace_once(
        text,
        '            f"{self.namespace}::{self.snapshot_function}"',
        '            f"::{self.namespace}::{self.snapshot_function}"',
        "qualified snapshot function",
    )

    filter_anchor = '''    if not count_matches or not snapshot_matches:
        return records

    for struct_match in structs:
'''
    filter_replacement = '''    if not count_matches or not snapshot_matches:
        return records

    aggregate_ranges = [
        (match.start(), match.end())
        for match in re.finditer(
            r"(?:struct|class)\\s+[A-Za-z_]\\w*\\s*(?:final\\s*)?\\{(.*?)\\}\\s*;",
            text,
            re.S,
        )
    ]

    def free_declaration(match: re.Match[str]) -> bool:
        return not any(start <= match.start() < end for start, end in aggregate_ranges)

    count_matches = [match for match in count_matches if free_declaration(match)]
    snapshot_matches = [match for match in snapshot_matches if free_declaration(match)]
    if not count_matches or not snapshot_matches:
        return records

    for struct_match in structs:
'''
    text = replace_once(text, filter_anchor, filter_replacement, "free-function filtering")

    text = replace_once(
        text,
        '''        snapshots = [match for match in snapshot_matches if match.group(2) == structure]
''',
        '''        snapshots = [
            match
            for match in snapshot_matches
            if match.group(2) == structure and namespace_at(text, match.start()) == namespace
        ]
''',
        "snapshot namespace matching",
    )
    text = replace_once(
        text,
        '''            ranked = sorted(
                count_matches,
''',
        '''            ranked = sorted(
                (
                    item
                    for item in count_matches
                    if namespace_at(text, item.start()) == namespace
                ),
''',
        "count namespace matching",
    )

    text = replace_once(
        text,
        '        "#include <cstddef>",\n        "#include <cstdio>",',
        '        "#include <cstddef>",\n        "#include <cstdint>",\n        "#include <cstdio>",',
        "generated cstdint include",
    )
    text = replace_once(
        text,
        '        "#include <string>",\n        "#include <vector>",',
        '        "#include <string>",\n        "#include <utility>",\n        "#include <vector>",',
        "generated utility include",
    )

    GENERATOR.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
