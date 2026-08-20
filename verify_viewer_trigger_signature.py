"""Verify the Shadowkeep native trigger-event observation boundary."""

from __future__ import annotations

import argparse
import mmap
from pathlib import Path

from verify_viewer_camera_signatures import file_to_rva, pe_layout, scan


EVENT_PATTERN = (
    "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 80 BB 9F 00 00 00 00 74 1A "
    "80 BB 9E 00 00 00 00 74 09 0F B6 83 9C 00 00 00 EB 02 B0 01 "
    "88 83 A4 00 00 00"
)
VOLUME_PATTERN = (
    "48 8B C4 55 48 8B EC 48 83 EC 70 44 8B 49 18 48 89 58 10 48 89 78 E8 33 FF "
    "4C 89 60 E0 4C 89 68 D8 4C 89 70 D0 4C 89 78 C8 4C 8B F9 48 89 7D C0 "
    "89 7D C8 C7 45 CC 00 00 00 80"
)
EXPECTED_EVENT_RVA = 0x136C1C0
EXPECTED_VOLUME_RVA = 0x1922490


def verify(path: Path) -> bool:
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
        image_base, sections = pe_layout(image)
        event_hits = scan(image, sections, EVENT_PATTERN)
        volume_hits = scan(image, sections, VOLUME_PATTERN)
        print(f"target={path}")
        print(f"image_base=0x{image_base:X}")
        print(f"trigger_event_tick.count={len(event_hits)}")
        print(f"trigger_volume_post_simulation.count={len(volume_hits)}")
        if len(event_hits) != 1 or len(volume_hits) != 1:
            return False
        event_rva = file_to_rva(event_hits[0], sections)
        volume_rva = file_to_rva(volume_hits[0], sections)
        print(f"trigger_event_tick.rva=0x{event_rva:X} expected=0x{EXPECTED_EVENT_RVA:X}")
        print(f"trigger_volume_post_simulation.rva=0x{volume_rva:X} expected=0x{EXPECTED_VOLUME_RVA:X}")
        return event_rva == EXPECTED_EVENT_RVA and volume_rva == EXPECTED_VOLUME_RVA


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    return 0 if verify(args.image) else 1


if __name__ == "__main__":
    raise SystemExit(main())
