from __future__ import annotations

import json
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
import build_activity_graph_catalog as converter  # noqa: E402


def write_fixture(path: Path, unknown_reference: bool = False) -> None:
    activity = {
        "1": {
            "displayProperties": {"name": "Synthetic Activity"},
            "activityGraphEntries": [{"activityGraphHash": 2}],
        }
    }
    graph = {
        "2": {
            "nodes": [
                {
                    "nodeHash": 3,
                    "position": {"x": 10.0, "y": 20.0},
                    "stateHash": 4,
                    "styleHash": 5,
                    "activityHashes": [999 if unknown_reference else 1],
                }
            ],
            "linkedGraphHashes": [],
            "connections": [],
        }
    }
    location = {
        "4": {
            "locationReleases": [
                {
                    "activityGraphHash": 2,
                    "activityGraphNodeHash": 3,
                    "spawnPoint": [1.0, 2.0, 3.0],
                    "publicPosition": [4.0, 5.0, 6.0, 7.0],
                }
            ]
        }
    }
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("DestinyActivityDefinition.json", json.dumps(activity))
        archive.writestr("DestinyActivityGraphDefinition.json", json.dumps(graph))
        archive.writestr("DestinyLocationDefinition.json", json.dumps(location))
        archive.writestr("manifest.json", json.dumps({"version": "87221.20.09.10.1506-2"}))


def test_minimal_conversion() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / "manifest.zip"
        output = root / "catalog.bin"
        write_fixture(source)
        summary = converter.convert(source, output, None, None)
        assert summary["content_build"] == 87221
        assert summary["graphs"] == 1
        assert summary["nodes"] == 1
        assert summary["activity_references"] == 1
        assert summary["linked_graphs"] == 0
        assert summary["locations"] == 1
        data = output.read_bytes()
        assert len(data) == struct.unpack_from("<I", data, 20)[0]
        fields = struct.unpack_from("<8s8I18I3I32s32s32sI", data, 0)
        assert fields[0] == b"SACAT001"
        assert fields[1] == 1
        sections = fields[9:27]
        assert sections[1 * 3 + 1] == 1  # graph count
        assert sections[2 * 3 + 1] == 1  # node count
        assert sections[5 * 3 + 1] == 1  # location count


def test_unknown_activity_reference_rejected() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / "manifest.zip"
        output = root / "catalog.bin"
        write_fixture(source, unknown_reference=True)
        try:
            converter.convert(source, output, 87221, "synthetic")
        except ValueError as error:
            assert "unknown activity" in str(error)
        else:
            raise AssertionError("unknown activity reference was accepted")


if __name__ == "__main__":
    test_minimal_conversion()
    test_unknown_activity_reference_rejected()
    print("ok")
