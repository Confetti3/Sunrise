"""Verify the Shadowkeep Wwise primary-listener copy boundary."""

from __future__ import annotations

import argparse
import mmap
from pathlib import Path

from verify_viewer_camera_signatures import decode_call, file_to_rva, pe_layout, rva_to_file, scan


PATTERNS = {
    "listener_command": (
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
        "8B F2 48 8B E9 E8 ? ? ? ? 48 8B 0D ? ? ? ? BA 10 00 00 00 "
        "44 0F B7 C0 E8 ? ? ? ?"
    ),
    "listener_wrapper": (
        "48 89 5C 24 10 57 48 81 EC 80 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 "
        "48 89 44 24 78 48 8B DA 0F BE F9 48 8B CB 4C 8D 44 24 40 "
        "48 8D 54 24 30 E8 ? ? ? ?"
    ),
}

EXPECTED_RVAS = {
    "listener_command": 0x19B60C0,
    "listener_wrapper": 0x1108E50,
}


def verify(path: Path) -> bool:
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
        image_base, sections = pe_layout(image)
        hits = {name: scan(image, sections, pattern) for name, pattern in PATTERNS.items()}
        print(f"target={path}")
        print(f"image_base=0x{image_base:X}")
        for name, offsets in hits.items():
            print(f"{name}.count={len(offsets)} file_offsets={','.join(hex(value) for value in offsets)}")
        if any(len(offsets) != 1 for offsets in hits.values()):
            return False

        rvas = {name: file_to_rva(offsets[0], sections) for name, offsets in hits.items()}
        for name, expected in EXPECTED_RVAS.items():
            actual = rvas[name]
            print(f"{name}.rva=0x{actual:X} expected=0x{expected:X} match={actual == expected}")
            if actual != expected:
                return False

        command_file = rva_to_file(rvas["listener_command"], sections)
        wrapper_file = rva_to_file(rvas["listener_wrapper"], sections)
        checks = {
            "command_type_listener_position_0x10": image[command_file + 0x25 : command_file + 0x2A]
            == bytes.fromhex("BA 10 00 00 00"),
            "command_reads_front_top_position": image[command_file + 0x33 : command_file + 0x38]
            == bytes.fromhex("F3 0F 10 5D 10"),
            "wrapper_signed_listener_index": image[wrapper_file + 0x1F : wrapper_file + 0x22]
            == bytes.fromhex("0F BE F9"),
            "wrapper_destiny_to_wwise_axis_map": image[wrapper_file + 0x5F : wrapper_file + 0x6B]
            == bytes.fromhex("66 0F 72 D1 19 66 0F 72 F1 17 0F 55"),
            "wrapper_calls_listener_command": decode_call(image, sections, wrapper_file + 0xF7)
            == rvas["listener_command"],
        }
        for name, passed in checks.items():
            print(f"{name}={passed}")
        return all(checks.values())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    return 0 if verify(args.image) else 1


if __name__ == "__main__":
    raise SystemExit(main())
