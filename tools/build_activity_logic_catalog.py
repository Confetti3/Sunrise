import argparse, csv, hashlib, io, re, struct, time, zipfile
from collections import defaultdict
from pathlib import Path

MAGIC=b"SLOGIC01"
SCHEMA=2
CONVERTER_VERSION=2
HEADER_SIZE=160
ACTIVITY_STRIDE=28
ENTITY_STRIDE=48
REF_STRIDE=4
PLACEMENT_STRIDE=44
EDGE_STRIDE=16
ROLE_IDS={
    "action_sequence":0,
    "action_target":1,
    "competitive_rule":2,
    "condition_monitor":3,
    "device":4,
    "object":5,
    "objective":6,
    "spatial_rule":7,
    "spawn_definition":8,
    "squad_definition":9,
    "trigger_source":10,
}
CONF_IDS={"strong":2,"probable":1}
ACTIVITY_RE=re.compile(r"^(.*?)\s+\[([0-9A-Fa-f]{8})\]$")

def read_csv(zf,name):
    with zf.open(name) as raw:
        text=io.TextIOWrapper(raw, encoding="utf-8-sig", newline="")
        yield from csv.DictReader(text)

def parse_u32_hex(s):
    return int(s,16) if s else 0
def parse_u64_hex(s):
    return int(s,16) if s else 0
def parse_f(s):
    return float(s) if s else 0.0

def build(input_path, output_path, source_format=None, content_build=None, generation_timestamp=None):
    input_path=Path(input_path)
    source_hasher=hashlib.sha256()
    with input_path.open('rb') as source_file:
        for chunk in iter(lambda: source_file.read(1024 * 1024), b''):
            source_hasher.update(chunk)
    source_digest=source_hasher.digest()
    if source_format is None:
        source_format="destiny2-static-activity-logic-archive-v2"
    if content_build is None:
        content_build=0
    if generation_timestamp is None:
        generation_timestamp=int(time.time())
    with zipfile.ZipFile(input_path) as zf:
        acts=[]
        for row in read_csv(zf,"activities/activity-archive-summaries.csv"):
            acts.append({
                "scenario":parse_u32_hex(row["scenario_tag"]),
                "name":row["activity"],
                "dest":row["destination"],
                "refs":[],
            })
        acts.sort(key=lambda a:(a["scenario"],a["name"]))
        act_by_tag={a["scenario"]:i for i,a in enumerate(acts)}
        if len(act_by_tag)!=len(acts):
            raise ValueError("duplicate scenario tag")

        defs=[]
        for row in read_csv(zf,"data/entity-definitions.csv"):
            pair=row["class_pair"].split("/",1)
            if len(pair)!=2:
                raise ValueError(f"bad class pair {row['class_pair']}")
            defs.append({
                "definition":parse_u32_hex(row["definition_tag"]),
                "class_primary":parse_u32_hex(pair[0]),
                "class_secondary":parse_u32_hex(pair[1]),
                "role":row["role"],
                "confidence":row["confidence"],
                "label":row["label"],
                "activities":row["activities"],
                "name":"",
                "localized":"",
                "placements":[],
            })
        defs.sort(key=lambda d:d["definition"])
        if len({d["definition"] for d in defs})!=len(defs):
            raise ValueError("duplicate definition tag")
        def_index={d["definition"]:i for i,d in enumerate(defs)}
        for i,d in enumerate(defs):
            for part in d["activities"].split("|"):
                part=part.strip()
                if not part: continue
                m=ACTIVITY_RE.match(part)
                if not m:
                    raise ValueError(f"bad activity reference: {part}")
                tag=int(m.group(2),16)
                ai=act_by_tag.get(tag)
                if ai is None:
                    raise ValueError(f"unknown activity scenario tag {tag:08X}")
                acts[ai]["refs"].append(i)

        best_names={}
        for row in read_csv(zf,"data/entity-name-hash-mappings.csv"):
            tag=parse_u32_hex(row["definition_tag"])
            if tag not in def_index:
                continue
            name=row["development_name"].strip()
            if not name:
                continue
            cur=best_names.get(tag)
            key=(len(name),name)
            if cur is None or key < (len(cur),cur):
                best_names[tag]=name
        for tag,name in best_names.items():
            defs[def_index[tag]]["name"]=name

        localized=defaultdict(set)
        for row in read_csv(zf,"data/logic-localized-string-candidates.csv"):
            tag=parse_u32_hex(row["source_definition"])
            if tag not in def_index:
                continue
            texts=row["candidate_texts"].strip()
            if not texts:
                continue
            # Candidate text field uses ' | ' when multiple decoded possibilities exist.
            if row.get("candidate_text_count","") == "1":
                localized[tag].add(texts)
        for tag,texts in localized.items():
            defs[def_index[tag]]["localized"]=" | ".join(sorted(texts)[:3])

        seen_placements=set()
        for row in read_csv(zf,"data/serialized-world-id-links.csv"):
            if row.get("confidence","") != "strong":
                continue
            tag=parse_u32_hex(row["source_definition"])
            ei=def_index.get(tag)
            if ei is None:
                continue
            world=parse_u64_hex(row["world_id"])
            pos=(parse_f(row["translation_x"]),parse_f(row["translation_y"]),parse_f(row["translation_z"]))
            rot=(parse_f(row["rotation_x"]),parse_f(row["rotation_y"]),parse_f(row["rotation_z"]),parse_f(row["rotation_w"]))
            key=(tag,world,pos,rot)
            if key in seen_placements:
                continue
            seen_placements.add(key)
            defs[ei]["placements"].append({
                "world":world,
                "map":parse_u32_hex(row["map_table_tag"]),
                "placed":parse_u32_hex(row["placed_entity_tag"]),
                "pos":pos,
                "rot":rot,
            })
        for d in defs:
            d["placements"].sort(key=lambda p:(p["world"],p["pos"]))

        edges=[]
        seen_edges=set()
        for row in read_csv(zf,"data/serialized-name-hash-edges.csv"):
            s=def_index.get(parse_u32_hex(row["source_definition"]))
            t=def_index.get(parse_u32_hex(row["target_definition"]))
            if s is None or t is None:
                continue
            name_hash=parse_u32_hex(row["name_hash"])
            occ=int(row.get("occurrence_count") or 0)
            key=(s,t,name_hash)
            if key in seen_edges:
                continue
            seen_edges.add(key)
            edges.append((s,t,name_hash,occ))
        edges.sort()

    strings=bytearray()
    string_map={}
    def intern(s):
        b=s.encode("utf-8")
        found=string_map.get(b)
        if found is not None:
            return found,len(b)
        off=len(strings)
        strings.extend(b)
        string_map[b]=off
        return off,len(b)

    source_format_off,source_format_len=intern(source_format)

    refs=[]
    act_records=[]
    for a in acts:
        name_off,name_len=intern(a["name"])
        dest_off,dest_len=intern(a["dest"])
        first=len(refs)
        uniq=sorted(set(a["refs"]))
        refs.extend(uniq)
        act_records.append((a["scenario"],name_off,name_len,dest_off,dest_len,first,len(uniq)))

    placements=[]
    entity_records=[]
    for d in defs:
        name=d["name"] or f"{d['label']} 0x{d['definition']:08X}"
        no,nl=intern(name)
        lo,ll=intern(d["label"])
        xo,xl=intern(d["localized"])
        first=len(placements)
        placements.extend(d["placements"])
        entity_records.append((
            d["definition"],d["class_primary"],d["class_secondary"],
            ROLE_IDS.get(d["role"],255),CONF_IDS.get(d["confidence"],0),
            no,nl,lo,ll,xo,xl,first,len(d["placements"])
        ))

    sections=[]
    cursor=HEADER_SIZE
    activities_off=cursor; cursor += len(act_records)*ACTIVITY_STRIDE
    entities_off=cursor; cursor += len(entity_records)*ENTITY_STRIDE
    refs_off=cursor; cursor += len(refs)*REF_STRIDE
    placements_off=cursor; cursor += len(placements)*PLACEMENT_STRIDE
    edges_off=cursor; cursor += len(edges)*EDGE_STRIDE
    strings_off=cursor; cursor += len(strings)
    total=cursor
    out=bytearray(total)
    out[0:8]=MAGIC
    struct.pack_into("<IIIII",out,8,SCHEMA,HEADER_SIZE,total,strings_off,len(strings))
    out[28:60]=source_digest
    struct.pack_into("<I",out,60,CONVERTER_VERSION)
    struct.pack_into("<IQII",out,124,content_build,generation_timestamp,source_format_off,source_format_len)
    section_desc=[
        (activities_off,len(act_records),ACTIVITY_STRIDE),
        (entities_off,len(entity_records),ENTITY_STRIDE),
        (refs_off,len(refs),REF_STRIDE),
        (placements_off,len(placements),PLACEMENT_STRIDE),
        (edges_off,len(edges),EDGE_STRIDE),
    ]
    off=64
    for desc in section_desc:
        struct.pack_into("<III",out,off,*desc); off+=12

    off=activities_off
    for rec in act_records:
        struct.pack_into("<7I",out,off,*rec); off+=ACTIVITY_STRIDE
    off=entities_off
    for rec in entity_records:
        definition,cp,cs,role,conf,no,nl,lo,ll,xo,xl,first,count=rec
        struct.pack_into("<IIIBBH8I",out,off,definition,cp,cs,role,conf,0,no,nl,lo,ll,xo,xl,first,count)
        off+=ENTITY_STRIDE
    off=refs_off
    for ei in refs:
        struct.pack_into("<I",out,off,ei); off+=4
    off=placements_off
    for p in placements:
        struct.pack_into("<QII7f",out,off,p["world"],p["map"],p["placed"],*p["pos"],*p["rot"])
        off+=PLACEMENT_STRIDE
    off=edges_off
    for e in edges:
        struct.pack_into("<4I",out,off,*e); off+=EDGE_STRIDE
    out[strings_off:strings_off+len(strings)]=strings
    Path(output_path).write_bytes(out)
    return {
        "activities":len(act_records),"entities":len(entity_records),"activity_refs":len(refs),
        "placements":len(placements),"edges":len(edges),"strings":len(strings),"bytes":len(out),
        "sha256":hashlib.sha256(out).hexdigest(),
    }

if __name__=="__main__":
    p=argparse.ArgumentParser()
    p.add_argument("--input",required=True)
    p.add_argument("--output",required=True)
    args=p.parse_args()
    print(build(args.input,args.output))
