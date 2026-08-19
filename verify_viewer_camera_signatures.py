"""Verify the detached Viewer Camera copy boundaries against the supported PE image."""

from __future__ import annotations

import argparse
import mmap
import re
import struct
from pathlib import Path


PATTERNS = {
    "camera_fov_copy": (
        "40 53 48 83 EC 20 E8 ? ? ? ? 48 8B D8 48 85 C0 74 1F "
        "F3 0F 10 80 D4 05 00 00 E8 ? ? ? ? 84 C0"
    ),
    "camera_pose_copy": (
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8 "
        "48 8B F2 48 8B D9 E8 ? ? ? ? 48 8B C8 48 85 C0"
    ),
    "render_camera_build": (
        "48 8B C4 48 89 58 10 55 48 8D 6C 24 80 48 81 EC 80 01 00 00 "
        "0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 44 0F 29 48 B8 "
        "44 0F 29 50 A8 44 0F 29 58 98"
    ),
    "camera_frame": (
        "4C 8B DC 41 57 48 81 EC B0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 "
        "48 89 44 24 38 49 89 6B 10 49 89 73 18"
    ),
    # The reconstructed runtime image was captured after the existing Teleport detour replaced the
    # first five bytes. The untouched remainder still identifies the exact native producer.
    "camera_transform_tail": (
        "48 89 74 24 18 48 89 7C 24 20 55 48 8D 6C 24 E0 "
        "48 81 EC 20 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 10 "
        "0F 57 C0 48 63 F9"
    ),
}

EXPECTED_RVAS = {
    "camera_fov_copy": 0x12D2280,
    "camera_pose_copy": 0x12D22C0,
    "render_camera_build": 0xB363A0,
    "camera_frame": 0x12D72B0,
    "camera_transform": 0x12D3B90,
}


def compile_pattern(text: str) -> re.Pattern[bytes]:
    pieces = (b"." if token == "?" else re.escape(bytes([int(token, 16)])) for token in text.split())
    return re.compile(b"".join(pieces), re.DOTALL)


def pe_layout(image: mmap.mmap) -> tuple[int, list[tuple[int, int, int, int, int]]]:
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError("input is not a PE image")
    section_count = struct.unpack_from("<H", image, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe_offset + 20)[0]
    optional_offset = pe_offset + 24
    if struct.unpack_from("<H", image, optional_offset)[0] != 0x20B:
        raise ValueError("input is not a PE32+ image")
    image_base = struct.unpack_from("<Q", image, optional_offset + 24)[0]
    table = optional_offset + optional_size
    sections = []
    for index in range(section_count):
        offset = table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", image, offset + 8
        )
        characteristics = struct.unpack_from("<I", image, offset + 36)[0]
        sections.append((virtual_address, virtual_size, raw_pointer, raw_size, characteristics))
    return image_base, sections


def executable_ranges(
    image: mmap.mmap, sections: list[tuple[int, int, int, int, int]]
) -> list[tuple[int, int]]:
    execute = 0x20000000
    return [
        (raw, min(len(image), raw + raw_size))
        for _rva, _virtual_size, raw, raw_size, characteristics in sections
        if characteristics & execute and raw < min(len(image), raw + raw_size)
    ]


def scan(
    image: mmap.mmap,
    sections: list[tuple[int, int, int, int, int]],
    pattern: str,
) -> list[int]:
    expression = compile_pattern(pattern)
    return [
        hit.start()
        for start, end in executable_ranges(image, sections)
        for hit in expression.finditer(image, start, end)
    ]


def file_to_rva(offset: int, sections: list[tuple[int, int, int, int, int]]) -> int:
    for virtual, _virtual_size, raw, raw_size, _characteristics in sections:
        if raw <= offset < raw + raw_size:
            return virtual + offset - raw
    raise ValueError(f"file offset 0x{offset:X} is outside mapped sections")


def rva_to_file(rva: int, sections: list[tuple[int, int, int, int, int]]) -> int:
    for virtual, virtual_size, raw, raw_size, _characteristics in sections:
        if virtual <= rva < virtual + max(virtual_size, raw_size):
            return raw + rva - virtual
    raise ValueError(f"RVA 0x{rva:X} is outside mapped sections")


def decode_call(
    image: mmap.mmap,
    sections: list[tuple[int, int, int, int, int]],
    instruction_file: int,
) -> int:
    if image[instruction_file] != 0xE8:
        raise ValueError(f"expected call at file offset 0x{instruction_file:X}")
    instruction_rva = file_to_rva(instruction_file, sections)
    displacement = struct.unpack_from("<i", image, instruction_file + 1)[0]
    return instruction_rva + 5 + displacement


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

        rvas = {
            name: file_to_rva(offsets[0], sections)
            for name, offsets in hits.items()
            if name != "camera_transform_tail"
        }
        # The tail starts five bytes after the patched/native entry.
        rvas["camera_transform"] = file_to_rva(hits["camera_transform_tail"][0], sections) - 5
        for name, expected in EXPECTED_RVAS.items():
            actual = rvas[name]
            print(f"{name}.rva=0x{actual:X} expected=0x{expected:X} match={actual == expected}")
            if actual != expected:
                return False

        fov_file = rva_to_file(rvas["camera_fov_copy"], sections)
        pose_file = rva_to_file(rvas["camera_pose_copy"], sections)
        render_file = rva_to_file(rvas["render_camera_build"], sections)
        frame_file = rva_to_file(rvas["camera_frame"], sections)

        checks = {
            "fov_field_0x5d4": image[fov_file + 0x13 : fov_file + 0x1B]
            == bytes.fromhex("F3 0F 10 80 D4 05 00 00"),
            "pose_position_0x594": image[pose_file + 0x29 : pose_file + 0x31]
            == bytes.fromhex("F2 0F 10 80 94 05 00 00"),
            "pose_forward_0x5bc": image[pose_file + 0x3E : pose_file + 0x46]
            == bytes.fromhex("F2 0F 10 81 BC 05 00 00"),
            "pose_up_0x5c8": image[pose_file + 0x53 : pose_file + 0x5B]
            == bytes.fromhex("F2 0F 10 81 C8 05 00 00"),
            "render_calls_fov_copy": decode_call(image, sections, render_file + 0x41)
            == rvas["camera_fov_copy"],
            "render_calls_pose_copy": decode_call(image, sections, render_file + 0x55)
            == rvas["camera_pose_copy"],
            "frame_source_handle_0x544": image[frame_file + 0x112 : frame_file + 0x118]
            == bytes.fromhex("89 83 44 05 00 00"),
            "frame_source_class_0x548": image[frame_file + 0x152 : frame_file + 0x158]
            == bytes.fromhex("89 8B 48 05 00 00"),
            "frame_calls_pose_producer": decode_call(image, sections, frame_file + 0x17F)
            == rvas["camera_transform"],
        }
        entry_file = rva_to_file(rvas["camera_transform"], sections)
        checks["camera_transform_native_or_detoured"] = image[entry_file] in (0x48, 0xE9)
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
