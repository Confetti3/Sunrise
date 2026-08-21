import csv
import hashlib
import io
import struct
import tempfile
import zipfile
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "build_activity_logic_catalog.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("logic_builder", TOOL)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_csv(zf, name, fieldnames, rows):
    text = io.StringIO(newline="")
    writer = csv.DictWriter(text, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(rows)
    zf.writestr(name, text.getvalue())


def make_fixture(path):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        write_csv(zf, "activities/activity-archive-summaries.csv",
                  ["activity", "scenario_tag", "destination", "entity_definitions",
                   "serialized_name_hash_edges", "serialized_world_id_links",
                   "localized_logic_strings", "role_counts"],
                  [{"activity": "test_encounter", "scenario_tag": "80ABCDEF",
                    "destination": "test_destination", "entity_definitions": "2",
                    "serialized_name_hash_edges": "1", "serialized_world_id_links": "1",
                    "localized_logic_strings": "1", "role_counts": "{}"}])
        write_csv(zf, "data/entity-definitions.csv",
                  ["definition_tag", "class_pair", "label", "role", "confidence",
                   "activities", "destinations", "source_resources", "string_count"],
                  [{"definition_tag": "80B50027", "class_pair": "808094CF/808094D0",
                    "label": "squad spawn rule", "role": "spawn_definition",
                    "confidence": "strong", "activities": "test_encounter [80ABCDEF]",
                    "destinations": "test_destination", "source_resources": "x", "string_count": "1"},
                   {"definition_tag": "80B50028", "class_pair": "808092AA/808092AB",
                    "label": "activity area/location", "role": "spatial_rule",
                    "confidence": "probable", "activities": "test_encounter [80ABCDEF]",
                    "destinations": "test_destination", "source_resources": "y", "string_count": "1"}])
        write_csv(zf, "data/entity-name-hash-mappings.csv",
                  ["definition_tag", "development_name"],
                  [{"definition_tag": "80B50027", "development_name": "sr_alpha[0]"},
                   {"definition_tag": "80B50028", "development_name": "cube"}])
        write_csv(zf, "data/logic-localized-string-candidates.csv",
                  ["source_definition", "candidate_text_count", "candidate_texts"],
                  [{"source_definition": "80B50028", "candidate_text_count": "1",
                    "candidate_texts": "Maevic Square"}])
        write_csv(zf, "data/serialized-world-id-links.csv",
                  ["confidence", "source_definition", "world_id", "map_table_tag",
                   "placed_entity_tag", "translation_x", "translation_y", "translation_z",
                   "rotation_x", "rotation_y", "rotation_z", "rotation_w"],
                  [{"confidence": "strong", "source_definition": "80B50027",
                    "world_id": "143BDCF3C15846BB", "map_table_tag": "80AA0001",
                    "placed_entity_tag": "80BB0001", "translation_x": "-112.20695",
                    "translation_y": "136.2643", "translation_z": "-7.9449",
                    "rotation_x": "0", "rotation_y": "0", "rotation_z": "0", "rotation_w": "1"}])
        write_csv(zf, "data/serialized-name-hash-edges.csv",
                  ["source_definition", "target_definition", "name_hash", "occurrence_count"],
                  [{"source_definition": "80B50027", "target_definition": "80B50028",
                    "name_hash": "81112233", "occurrence_count": "1"}])


def main():
    module = load_tool()
    with tempfile.TemporaryDirectory() as td:
        source = Path(td) / "fixture.zip"
        output = Path(td) / "logic.bin"
        make_fixture(source)
        result = module.build(source, output)
        assert result["activities"] == 1
        assert result["entities"] == 2
        assert result["activity_refs"] == 2
        assert result["placements"] == 1
        assert result["edges"] == 1
        data = output.read_bytes()
        assert data[:8] == b"SLOGIC01"
        schema, header, total, strings_off, strings_size = struct.unpack_from("<IIIII", data, 8)
        assert schema == 2 and header == 160 and total == len(data)
        assert strings_off + strings_size == len(data)
        converter_version = struct.unpack_from("<I", data, 60)[0]
        content_build, generation_timestamp, fmt_off, fmt_len = struct.unpack_from("<IQII", data, 124)
        assert converter_version == 2
        assert content_build == 0
        assert generation_timestamp > 0
        fmt = data[strings_off + fmt_off: strings_off + fmt_off + fmt_len].decode("utf-8")
        assert fmt == "destiny2-static-activity-logic-archive-v2"
        digest = data[28:60]
        assert digest == hashlib.sha256(source.read_bytes()).digest()
        activity_off, activity_count, activity_stride = struct.unpack_from("<III", data, 64)
        assert activity_count == 1 and activity_stride == 28
        entity_off, entity_count, entity_stride = struct.unpack_from("<III", data, 76)
        assert entity_count == 2 and entity_stride == 48
        scenario = struct.unpack_from("<I", data, activity_off)[0]
        assert scenario == 0x80ABCDEF
        definition = struct.unpack_from("<I", data, entity_off)[0]
        assert definition == 0x80B50027
    print("activity logic converter tests passed")


if __name__ == "__main__":
    main()
