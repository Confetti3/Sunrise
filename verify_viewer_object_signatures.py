"""Verify build-86657 occupied object-datum iterator production signatures."""

from __future__ import annotations

import argparse
import mmap
from pathlib import Path

from verify_viewer_camera_signatures import file_to_rva, pe_layout, scan


SIGNATURES = (
    (
        "finish_datum",
        "48 89 5C 24 ? 57 48 83 EC 20 8B F9 8B D9 81 E7 FF 1F 00 00 0F AF 3D ? ? ? ? "
        "48 03 3D ? ? ? ? E8",
        0x56C6B0,
    ),
    (
        "iterator_init",
        "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC 20 "
        "48 8B 05 ? ? ? ? 48 8D 2D ? ? ? ?",
        0x562530,
    ),
    (
        "iterator_next",
        "40 53 48 83 EC 20 48 8B D9 48 83 C1 08 E8 ? ? ? ? 48 8B CB 48 83 C4 20 "
        "5B E9 ? ? ? ?",
        0x564140,
    ),
)


def verify(path: Path) -> bool:
    valid = True
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
        image_base, sections = pe_layout(image)
        print(f"target={path}")
        print(f"image_base=0x{image_base:X}")
        for name, pattern, expected_rva in SIGNATURES:
            hits = scan(image, sections, pattern)
            print(f"{name}.count={len(hits)}")
            if len(hits) != 1:
                valid = False
                continue
            rva = file_to_rva(hits[0], sections)
            print(f"{name}.rva=0x{rva:X} expected=0x{expected_rva:X}")
            valid = valid and rva == expected_rva
    return valid


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    return 0 if verify(args.image) else 1


if __name__ == "__main__":
    raise SystemExit(main())
