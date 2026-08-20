#!/usr/bin/env python3
"""Convert selected Destiny activity manifest tables into Sunrise's compact catalog."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import zipfile
from pathlib import Path
from typing import Any, Iterable

MAGIC = b"SACAT001"
SCHEMA_VERSION = 1
HEADER_SIZE = 224
MAX_FILE_SIZE = 256 * 1024 * 1024


def as_int(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            try:
                return int(value)
            except ValueError:
                return None
    return None


def first(record: dict[str, Any], *names: str) -> Any:
    for name in names:
        if name in record:
            return record[name]
    return None


def nested(record: dict[str, Any], *paths: tuple[str, ...]) -> Any:
    for path in paths:
        value: Any = record
        for key in path:
            if not isinstance(value, dict) or key not in value:
                value = None
                break
            value = value[key]
        if value is not None:
            return value
    return None


def records(table: Any) -> Iterable[tuple[int, dict[str, Any]]]:
    if isinstance(table, list):
        for index, value in enumerate(table):
            if isinstance(value, dict):
                key = as_int(first(value, "hash", "activityHash", "graphHash", "locationHash"))
                yield (key if key is not None else index, value)
        return
    if isinstance(table, dict):
        response = table.get("Response")
        if response is not None:
            yield from records(response)
            return
        for key, value in table.items():
            if not isinstance(value, dict):
                continue
            numeric = as_int(key)
            if numeric is None:
                numeric = as_int(first(value, "hash", "activityHash", "graphHash", "locationHash"))
            if numeric is not None:
                yield numeric, value


def name_of(record: dict[str, Any]) -> str:
    value = nested(record, ("displayProperties", "name"), ("display", "name"), ("activityName",), ("name",), ("displayName",))
    return value if isinstance(value, str) else ""


def scalar_hash(value: Any) -> int | None:
    if isinstance(value, dict):
        return as_int(first(value, "hash", "activityHash", "graphHash", "nodeHash", "linkedGraphHash"))
    return as_int(value)


def hash_list(record: dict[str, Any], *names: str) -> list[int]:
    values: list[Any] = []
    for name in names:
        value = record.get(name)
        if value is None:
            continue
        values.extend(value if isinstance(value, list) else [value])
    result: list[int] = []
    for value in values:
        numeric = scalar_hash(value)
        if numeric is not None and numeric != 0 and numeric not in result:
            result.append(numeric)
    return result


def position(value: Any, lanes: int) -> list[float] | None:
    if isinstance(value, dict):
        names = ("x", "y", "z", "w")
        result = [value.get(name) for name in names[:lanes]]
    elif isinstance(value, (list, tuple)):
        result = list(value[:lanes])
    else:
        return None
    if len(result) != lanes or any(not isinstance(lane, (int, float)) for lane in result):
        return None
    result = [float(lane) for lane in result]
    return result if all(math.isfinite(lane) for lane in result) else None


def node_list(record: dict[str, Any]) -> list[dict[str, Any]]:
    value = first(record, "nodes", "graphNodes", "activityGraphNodes")
    if not isinstance(value, list):
        return []
    return [node for node in value if isinstance(node, dict)]


def load_table(archive: zipfile.ZipFile, table_name: str) -> tuple[Any, bytes]:
    candidates = [name for name in archive.namelist() if name.endswith("/" + table_name) or name == table_name]
    if not candidates:
        raise ValueError(f"manifest is missing {table_name}")
    raw = archive.read(candidates[0])
    return json.loads(raw.decode("utf-8")), raw


def extract_version(archive: zipfile.ZipFile, fallback: str) -> str:
    for name in archive.namelist():
        if not name.lower().endswith(("manifest.json", "package.json", "version.json")):
            continue
        try:
            value = json.loads(archive.read(name).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if isinstance(value, dict):
            candidate = first(value, "contentVersion", "manifestVersion", "version")
            if isinstance(candidate, str) and candidate:
                return candidate
    return fallback


def content_build(version: str, override: int | None) -> int:
    if override is not None:
        return override
    match = re.search(r"\d+", version)
    return int(match.group(0)) if match else 0


def build_records(activity_table: Any, graph_table: Any, location_table: Any) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[tuple[int, int, int, list[float], list[float]]]]:
    activities: list[dict[str, Any]] = []
    for key, record in records(activity_table):
        graph_hashes = hash_list(record, "activityGraphHashes", "graphHashes", "activityGraphHash")
        entries = first(record, "activityGraphEntries", "graphEntries")
        if isinstance(entries, list):
            for entry in entries:
                if isinstance(entry, dict):
                    value = scalar_hash(first(entry, "activityGraphHash", "graphHash", "hash"))
                else:
                    value = scalar_hash(entry)
                if value is not None and value != 0 and value not in graph_hashes:
                    graph_hashes.append(value)
        activities.append({"hash": key, "name": name_of(record), "graphs": graph_hashes})
    activities.sort(key=lambda row: row["hash"])

    graphs: list[dict[str, Any]] = []
    for key, record in records(graph_table):
        nodes: list[dict[str, Any]] = []
        for index, node in enumerate(node_list(record), start=1):
            node_hash = as_int(first(node, "nodeHash", "hash", "id", "nodeIndex")) or index
            authored = position(first(node, "position", "nodePosition", "nodeDisplayPosition"), 2)
            if authored is None:
                x = first(node, "positionX", "x")
                y = first(node, "positionY", "y")
                if not (isinstance(x, (int, float)) and isinstance(y, (int, float))):
                    raise ValueError(f"graph node {node_hash} lacks an authored position")
                authored = [float(x), float(y)]
                if not all(math.isfinite(lane) for lane in authored):
                    raise ValueError(f"graph node {node_hash} has a non-finite authored position")
            activity_hashes = hash_list(node, "activityHashes", "activities", "activityHash")
            linked = hash_list(node, "linkedGraphHashes", "linkedGraphs", "linkedGraphHash")
            nodes.append({
                "graph": key,
                "node": node_hash,
                "x": authored[0],
                "y": authored[1],
                "state": as_int(first(node, "stateHash", "nodeStateHash")) or 0,
                "style": as_int(first(node, "styleHash", "nodeStyleHash")) or 0,
                "activities": activity_hashes,
                "linked": linked,
            })
        nodes.sort(key=lambda row: row["node"])
        graphs.append({
            "hash": key,
            "nodes": nodes,
            "linked": hash_list(record, "linkedGraphHashes", "linkedGraphs", "linkedGraphHash"),
        })
    graphs.sort(key=lambda row: row["hash"])

    locations: list[tuple[int, int, int, list[float], list[float]]] = []
    for location_hash, record in records(location_table):
        releases = first(record, "locationReleases", "releases")
        if not isinstance(releases, list):
            releases = [record]
        for release in releases:
            if not isinstance(release, dict):
                continue
            graph_hash = scalar_hash(first(release, "activityGraphHash", "graphHash"))
            node_hash = scalar_hash(first(release, "activityGraphNodeHash", "nodeHash", "graphNodeHash"))
            if not graph_hash or not node_hash:
                continue
            spawn = position(first(release, "spawnPoint", "spawnPosition", "position"), 3) or [0.0, 0.0, 0.0]
            public = position(first(release, "publicPosition", "publicFourLanePosition", "fourLanePosition"), 4) or [0.0, 0.0, 0.0, 0.0]
            locations.append((location_hash, graph_hash, node_hash, spawn, public))
    locations.sort(key=lambda value: (value[1], value[2], value[0]))
    return activities, graphs, locations


def validate_records(activities: list[dict[str, Any]], graphs: list[dict[str, Any]], locations: list[tuple[int, int, int, list[float], list[float]]]) -> None:
    activity_hashes = {row["hash"] for row in activities}
    graph_hashes = {row["hash"] for row in graphs}
    if 0 in activity_hashes or 0 in graph_hashes or len(activity_hashes) != len(activities) or len(graph_hashes) != len(graphs):
        raise ValueError("duplicate or zero activity/graph hash")
    node_keys: set[tuple[int, int]] = set()
    for activity in activities:
        unknown = set(activity["graphs"]) - graph_hashes
        if unknown:
            raise ValueError(f"activity {activity['hash']} references unknown graph {min(unknown)}")
    for graph in graphs:
        for linked in graph["linked"]:
            if linked not in graph_hashes:
                raise ValueError(f"graph {graph['hash']} links unknown graph {linked}")
        for node in graph["nodes"]:
            key = (node["graph"], node["node"])
            if node["node"] == 0 or key in node_keys:
                raise ValueError("duplicate graph node key")
            node_keys.add(key)
            unknown_activities = set(node["activities"]) - activity_hashes
            if unknown_activities:
                raise ValueError(f"node {node['node']} references unknown activity {min(unknown_activities)}")
            unknown_links = set(node["linked"]) - graph_hashes
            if unknown_links:
                raise ValueError(f"node {node['node']} links unknown graph {min(unknown_links)}")
            if not all(math.isfinite(float(node[lane])) for lane in ("x", "y")):
                raise ValueError("non-finite authored position")
    for _, graph_hash, node_hash, spawn, public in locations:
        if (graph_hash, node_hash) not in node_keys:
            raise ValueError("location release references unknown graph node")
        if not all(math.isfinite(float(value)) for value in spawn + public):
            raise ValueError("non-finite location position")


def build_binary(version: str, build: int, digests: tuple[bytes, bytes, bytes], activities: list[dict[str, Any]], graphs: list[dict[str, Any]], locations: list[tuple[int, int, int, list[float], list[float]]]) -> bytes:
    strings = bytearray()
    string_offsets: dict[str, tuple[int, int]] = {}

    def string_ref(value: str) -> tuple[int, int]:
        if value not in string_offsets:
            offset = len(strings)
            encoded = value.encode("utf-8")
            strings.extend(encoded)
            strings.append(0)
            string_offsets[value] = (offset, len(encoded))
        return string_offsets[value]

    version_ref = string_ref(version)
    activity_refs: list[int] = []
    activity_rows: list[tuple[int, int, int, int, int]] = []
    for activity in activities:
        start = len(activity_refs)
        activity_refs.extend(activity["graphs"])
        name_offset, name_length = string_ref(activity["name"])
        activity_rows.append((activity["hash"], name_offset, name_length, start, len(activity["graphs"])))

    linked_refs: list[int] = []
    graph_rows: list[tuple[int, int, int, int, int]] = []
    node_rows: list[tuple[int, int, float, float, int, int, int, int, int, int]] = []
    for graph in graphs:
        node_start = len(node_rows)
        linked_start = len(linked_refs)
        linked_refs.extend(graph["linked"])
        for node in graph["nodes"]:
            activity_start = len(activity_refs)
            activity_refs.extend(node["activities"])
            node_linked_start = len(linked_refs)
            linked_refs.extend(node["linked"])
            node_rows.append((node["graph"], node["node"], node["x"], node["y"], node["state"], node["style"], activity_start, len(node["activities"]), node_linked_start, len(node["linked"])))
        graph_rows.append((graph["hash"], node_start, len(graph["nodes"]), linked_start, len(graph["linked"])))

    section_counts = [len(activity_rows), len(graph_rows), len(node_rows), len(activity_refs), len(linked_refs), len(locations)]
    section_strides = [20, 20, 40, 4, 4, 40]
    section_offsets: list[int] = []
    offset = HEADER_SIZE
    for count, stride in zip(section_counts, section_strides):
        section_offsets.append(offset)
        offset += count * stride
    string_offset = offset
    total_size = string_offset + len(strings)
    if total_size > MAX_FILE_SIZE:
        raise ValueError("catalog output exceeds the supported maximum")

    def absolute_string(ref: tuple[int, int]) -> tuple[int, int]:
        return string_offset + ref[0], ref[1]

    activity_bytes = b"".join(struct.pack("<5I", row[0], *absolute_string(string_ref(next(activity["name"] for activity in activities if activity["hash"] == row[0]))), row[3], row[4]) for row in activity_rows)
    graph_bytes = b"".join(struct.pack("<5I", *row) for row in graph_rows)
    node_bytes = b"".join(struct.pack("<2I2f6I", *row) for row in node_rows)
    activity_ref_bytes = b"".join(struct.pack("<I", value) for value in activity_refs)
    linked_ref_bytes = b"".join(struct.pack("<I", value) for value in linked_refs)
    location_bytes = b"".join(struct.pack("<3I7f", location_hash, graph_hash, node_hash, *spawn, *public) for location_hash, graph_hash, node_hash, spawn, public in locations)

    section_fields: list[int] = []
    for section_offset, count, stride in zip(section_offsets, section_counts, section_strides):
        section_fields.extend((section_offset, count, stride))
    header = struct.pack(
        "<8s8I18I3I32s32s32sI",
        MAGIC,
        SCHEMA_VERSION,
        build,
        HEADER_SIZE,
        total_size,
        string_offset + version_ref[0],
        version_ref[1],
        string_offset,
        len(strings),
        *section_fields,
        0,
        0,
        0,
        digests[0],
        digests[1],
        digests[2],
        0,
    )
    if len(header) != HEADER_SIZE:
        raise ValueError(f"internal header size mismatch: {len(header)}")
    return header + activity_bytes + graph_bytes + node_bytes + activity_ref_bytes + linked_ref_bytes + location_bytes + bytes(strings)


def convert(input_path: Path, output_path: Path, build_override: int | None, version_override: str | None) -> dict[str, int | str]:
    with zipfile.ZipFile(input_path, "r") as archive:
        activity_table, activity_raw = load_table(archive, "DestinyActivityDefinition.json")
        graph_table, graph_raw = load_table(archive, "DestinyActivityGraphDefinition.json")
        location_table, location_raw = load_table(archive, "DestinyLocationDefinition.json")
        version = version_override or extract_version(archive, input_path.name)
        build = content_build(version, build_override)
        if build == 0:
            raise ValueError("numeric content build is unavailable; pass --content-build")
        activities, graphs, locations = build_records(activity_table, graph_table, location_table)
        validate_records(activities, graphs, locations)
        data = build_binary(
            version,
            build,
            (hashlib.sha256(activity_raw).digest(), hashlib.sha256(graph_raw).digest(), hashlib.sha256(location_raw).digest()),
            activities,
            graphs,
            locations,
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(data)
    return {"content_build": build, "version": version, "graphs": len(graphs), "nodes": sum(len(graph["nodes"]) for graph in graphs), "activity_references": sum(len(node["activities"]) for graph in graphs for node in graph["nodes"]), "linked_graphs": sum(len(graph["linked"]) + sum(len(node["linked"]) for node in graph["nodes"]) for graph in graphs), "locations": len(locations), "bytes": len(data)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--content-build", type=int)
    parser.add_argument("--version")
    args = parser.parse_args()
    summary = convert(args.input, args.output, args.content_build, args.version)
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
