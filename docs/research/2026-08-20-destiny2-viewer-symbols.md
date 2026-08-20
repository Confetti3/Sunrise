# Destiny 2 Viewer symbol research

> Analysis date: 2026-08-20
>
> Scope: offline analysis of historical Destiny 2 binaries supplied by the project owner
>
> Tools: `dumpbin`, `llvm-pdbutil`, IDA Pro 9.0, Python, Capstone 5, and the repository signature
> verifiers

This report records a symbol-rich build-87221 executable/PDB pair and the functions that were
uniquely migrated to Sunrise's build-86657 target. The migrations are research anchors; they are not
production hook approvals.

## Scope and inputs

No game process was launched, controlled, or modified during this analysis. The supplied `.bin` and
PDB were read-only inputs. Sunrise targets the separate build-86657 image.

| Input | SHA-256 | Identity |
| --- | --- | --- |
| `destiny2_dethread_pdata.bin` | `FBAAE84129D1080157225D2E8D12A59E1D389DBA4C3736800A3CF790B36C833D` | Complete x64 PE, `87221.20.09.10.1506.d2_live`, timestamp `2020-09-10 18:12:06` |
| `tiger_release_final.pdb` | `16713656EE7F336EBD1B5866A23EA3D41F07815082DDB8F206E3AEF16B57B4B6` | PDB v7, GUID `{17D16314-31E5-41E1-AB9A-909B0BC13F7D}`, age 1 |
| Build-86657 analysis image | `36A04F76B8A7D0778EB2A45CF05688A26817175C67D7DD43375743CA016B65A4` | `86657.20.08.23.1800.d2_rc`, GUID `{0DDCFBDF-EB68-4148-8BFB-7C7618FFAB03}`, age 1 |

The file named `destiny2_dethread_pdata.bin` is a complete PE image, not a raw `.pdata` section.
Its embedded RSDS identity matches the supplied PDB exactly. That PDB does not match build 86657;
the two client images were produced 18 days apart.

## Evidence

| ID | Source | Reproduction | Content hash |
| --- | --- | --- | --- |
| E-001 | PE headers, version resources, and RSDS records | Run `dumpbin /headers` on both executable images and `llvm-pdbutil dump -summary` on the PDB | Input SHA-256 values above |
| E-002 | PDB public/module streams | Run `llvm-pdbutil dump -publics -modules -files` and count public names by prefix/keyword | PDB SHA-256 above |
| E-003 | `.pdata` function corpus and normalized Capstone instruction matching | Resolve each build-87221 symbol to its enclosing runtime function, normalize address-bearing operands, and require one build-86657 match for the first 16 instructions and the complete instruction stream | Both executable SHA-256 values above |
| E-004 | Repository signature verifiers | Run the camera, audio, and trigger verifier commands documented below | Build-86657 SHA-256 above |

`llvm-pdbutil` reported 128,442 public symbols. Of those, 52,690 retained generic `FUN_...`
names and approximately 75,752 had curated names.

| Public-name area | Matches |
| --- | ---: |
| Entity | 2,054 |
| Render / gfx | 2,211 |
| Audio | 1,799 |
| Physics / Havok | 1,032 |
| Light | 203 |
| Trigger | 142 |
| Terrain | 29 |

IDA loaded 128,440 symbols. It accepted 112,977 of 113,805 runtime-function entries and skipped 828
entries with invalid unwind data.

## Findings

| ID | Finding | Evidence | Confidence | Location | Status |
| --- | --- | --- | --- | --- | --- |
| F-001 | The `.bin` is a complete build-87221 PE and the PDB is its exact GUID/age match. | E-001 | High | PE debug directory and PDB info stream | Verified |
| F-002 | The PDB is most consistent with an analysis-generated symbol export, not Bungie's original private compiler PDB. It has one synthetic `destiny2.obj` module, no source files, no IPI stream, and no usable private types. | E-002 | High | PDB module, file, and type streams | Verified inference |
| F-003 | Six high-value build-87221 functions have unique, instruction-equivalent matches in build 86657. | E-003 | High | RVAs in the migration table | Verified offline |
| F-004 | The symbol set exposes promising entity, trigger, audio-emitter, terrain, light, rendering, physics, and Havok boundaries. | E-002 | High | PDB public stream | Verified names; semantics require call-site review |
| F-005 | A migrated RVA alone does not establish a safe Sunrise producer. Caller thread, ownership, object layout, transition lifetime, and a pointer-free publication boundary remain required. | E-003, E-004 | High | Sunrise runtime lifecycle | Enforced design constraint |

## Verified symbol migrations

Each row produced exactly one build-86657 candidate. The complete normalized instruction stream and
function length matched; raw-byte differences were limited primarily to address-bearing operands.

| Build-87221 symbol | 87221 RVA | Build-86657 RVA | Raw byte agreement |
| --- | ---: | ---: | ---: |
| `world_entity_array_foreach` | `0xD28FE0` | `0xD2AC60` | 58 / 62 |
| `world_check_proximity_and_trigger` | `0x4AD670` | `0x4AEB10` | 80 / 82 |
| `trigger_volume_contains_point` | `0x107A7C0` | `0x107DD90` | 186 / 194 |
| `trigger_notification_if_in_range` | `0xABA1F0` | `0xABC930` | 225 / 248 |
| `audio_emitter_slot_alloc` | `0xD84FA0` | `0xD86930` | 74 / 77 |
| `world_find_first_active_object` | `0x11D7E20` | `0x11DB920` | 107 / 108 |

The first follow-up research anchors are `world_entity_array_foreach` for broader identity/call-flow
analysis and `audio_emitter_slot_alloc` for emitter lifecycle analysis. The trigger mappings are
useful for authored containment/call-flow work, but Sunrise's current trigger producer already uses
separately verified native event and Havok post-simulation boundaries.

## Solve path

**P-001 — cross-build symbol migration** (`path_type=solve`)

1. E-001 established the exact identity of each executable and proved which PDB belongs to which
   image.
2. E-002 extracted the curated build-87221 symbol/address map.
3. E-003 used `.pdata` to recover function boundaries and normalized disassembly to remove relocated
   address noise.
4. Each reference function was matched against the complete build-86657 function corpus.
5. Only unique full-stream matches were accepted into the migration table.
6. F-005 keeps those mappings in research status until a native lifecycle and pointer-free snapshot
   design is independently proven.

## Reproducing the evidence

Run these commands from a Visual Studio Developer PowerShell with LLVM's `bin` directory on `PATH`.
Enter local paths interactively so the commands do not depend on one workstation layout:

```powershell
$Image87221 = Read-Host 'Path to destiny2_dethread_pdata.bin'
$Pdb87221 = Read-Host 'Path to tiger_release_final.pdb'
$Image86657 = Read-Host 'Path to the exact build-86657 Destiny 2 executable'

Get-FileHash -Algorithm SHA256 -LiteralPath $Image87221, $Pdb87221, $Image86657
dumpbin /headers $Image87221
dumpbin /headers $Image86657
llvm-pdbutil dump -summary $Pdb87221
llvm-pdbutil dump -modules -files $Pdb87221
llvm-pdbutil dump -publics $Pdb87221

python .\verify_viewer_camera_signatures.py $Image86657
python .\verify_viewer_audio_signatures.py $Image86657
python .\verify_viewer_trigger_signature.py $Image86657
```

To reproduce E-003 independently, load both images in a disassembler, resolve each build-87221 RVA
to its `.pdata` function, and compare it with the stated build-86657 RVA after masking relative
branches, absolute immediates, and RIP-relative displacements. Reject a mapping if it is not unique
or if the complete normalized instruction stream differs.

## Timeline

| Time (UTC) | Event |
| --- | --- |
| 2026-08-20 15:30 | Hashed and identified both supplied files and the build-86657 analysis image. |
| 2026-08-20 15:35 | Confirmed the PDB/PE RSDS match and the cross-build mismatch. |
| 2026-08-20 15:40 | Counted public symbols and classified the PDB as an analysis export. |
| 2026-08-20 15:50 | Loaded the symbolized build-87221 image in IDA and confirmed symbol coverage. |
| 2026-08-20 15:55 | Produced six unique normalized function matches in build 86657. |
| 2026-08-20 16:00 | Recorded lifecycle limitations and retained the mappings as offline research evidence. |

## Remaining research

- Trace callers and data ownership around `world_entity_array_foreach` and
  `world_find_first_active_object`; do not assume either returns durable simulation identities.
- Establish emitter identity, transform, thread ownership, and teardown behavior around
  `audio_emitter_slot_alloc` before publishing audio-emitter nodes.
- Locate actual terrain and light instance producers. Global feature/type symbols do not enumerate
  live instances by themselves.
- Recover proven mesh-resource and bounds layouts before claiming geometry beyond live object-type
  classification.
