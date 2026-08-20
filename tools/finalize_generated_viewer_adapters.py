"""Finalize and validate the generated Viewer catalog adapter header."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "Sunrise/src/client/inspection/providers/generated_runtime_catalog_adapters.h"


def finalize(text: str) -> str:
    for include in ("#include <cstdint>\n", "#include <utility>\n"):
        if include not in text:
            anchor = "#include <cstddef>\n"
            if anchor not in text:
                raise RuntimeError("generated adapter include block is missing")
            text = text.replace(anchor, anchor + include, 1)
    return text


def validate(text: str) -> None:
    required = (
        "namespace sunrise::client::inspection::providers::generated",
        "inline void append_runtime_catalogs(",
        "NodeId parent",
        "std::vector<Diagnostic>& diagnostics",
    )
    for value in required:
        if value not in text:
            raise RuntimeError(f"generated adapter contract is missing: {value}")

    adapter_keys = re.findall(
        r"const\s+([A-Za-z_:]\w*(?:::\w+)*)&\s+record\s*=\s*records\[ordinal\]",
        text,
    )
    duplicates = sorted({value for value in adapter_keys if adapter_keys.count(value) > 1})
    if duplicates:
        raise RuntimeError("duplicate generated adapters: " + ", ".join(duplicates))

    if "reinterpret_cast" in text or "0x" in text:
        raise RuntimeError("generated adapters must not introduce memory offsets or reinterpret casts")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if not HEADER.exists():
        raise RuntimeError("catalog adapter generator did not create its header")
    original = HEADER.read_text(encoding="utf-8")
    updated = finalize(original)
    validate(updated)
    if args.check and updated != original:
        return 1
    HEADER.write_text(updated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
