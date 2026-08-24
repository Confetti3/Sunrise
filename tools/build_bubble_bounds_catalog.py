"""Builds bubble-bounds-catalog.bin from mesh-hillshade bubble footprint records.

Input: the research corpus directory `reports/activity-logic/mesh-hillshade-bubbles`
(one JSON per bubble: tag, family, package-derived world_bounds AABB, provenance).
Output: the packed little-endian catalog the Sunrise inspector loads as its first
proven bounds producer. Only records whose world_bounds is a finite, well-ordered
AABB are retained; everything else is skipped and reported. No values are invented.
"""
import argparse, json, struct
from pathlib import Path

MAGIC = b"SBBCT001"
SCHEMA = 1
HEADER_SIZE = 24
RECORD_SIZE = 64
FAMILY_CAPACITY = 32
TAG_CLASS_MASK = 0xFFF0_0000_0000_0000


def load_bubbles(corpus):
    root = Path(corpus) / "mesh-hillshade-bubbles"
    bubbles = []
    skipped = 0
    for family_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        for record_path in sorted(family_dir.glob("*.json")):
            try:
                record = json.loads(record_path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                skipped += 1
                continue
            tag_text = record_path.stem
            bounds = record.get("world_bounds")
            if not isinstance(bounds, list) or len(bounds) != 6:
                skipped += 1
                continue
            try:
                minimum = [float(v) for v in bounds[:3]]
                maximum = [float(v) for v in bounds[3:]]
            except (TypeError, ValueError):
                skipped += 1
                continue
            if not all(
                v == v and abs(v) != float("inf") for v in minimum + maximum
            ):
                skipped += 1
                continue
            if any(minimum[i] > maximum[i] for i in range(3)):
                skipped += 1
                continue
            try:
                tag = int(tag_text, 16)
            except ValueError:
                skipped += 1
                continue
            if tag == 0 or tag & TAG_CLASS_MASK:
                skipped += 1
                continue
            family = family_dir.name[: FAMILY_CAPACITY - 1]
            if not family:
                skipped += 1
                continue
            bubbles.append((tag, family, minimum, maximum))
    return bubbles, skipped


def build(corpus, output_path, content_build):
    bubbles, skipped = load_bubbles(corpus)
    if not bubbles:
        raise SystemExit("no valid bubble footprints found under the corpus")
    out = bytearray()
    out += MAGIC
    out += struct.pack("<IIII", SCHEMA, content_build, len(bubbles), 0)
    for tag, family, minimum, maximum in bubbles:
        out += struct.pack("<Q", tag)
        encoded = family.encode("utf-8")[: FAMILY_CAPACITY - 1]
        out += encoded + b"\0" * (FAMILY_CAPACITY - len(encoded))
        out += struct.pack("<6f", *minimum, *maximum)
    assert len(out) == HEADER_SIZE + RECORD_SIZE * len(bubbles)
    Path(output_path).write_bytes(out)
    print(
        f"wrote {len(bubbles)} bubble footprints ({skipped} skipped) "
        f"to {output_path} for content build {content_build}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--corpus",
        required=True,
        help="directory containing mesh-hillshade-bubbles (reports/activity-logic)",
    )
    parser.add_argument("--output", required=True, help="path of the .bin catalog")
    parser.add_argument("--content-build", type=int, default=86657)
    args = parser.parse_args()
    build(args.corpus, args.output, args.content_build)


if __name__ == "__main__":
    main()
