# Conformance gaps: SAM3DBody-flavoured ARF vs. ISO/IEC 23090-39

This is the scoping note for turning `libarf` from a reader/writer of
SAM3DBody-flavoured ARF into a reader/writer of conformant ARF. It exists so
the rewrite has a concrete target to diff against, and so the differences are
written down once rather than rediscovered file by file.

Field names, types, and mandatory/optional status below are facts about the
format and are stated plainly. Descriptions are paraphrased in this project's
own words. No schema or bitstream-syntax text is quoted from ISO/IEC 23090-39;
consistent with [`doc/ARF.md`](ARF.md), no specification text is reproduced
here. The source used to compile this note is the FDIS-stage text as
currently published at the MPEG ARF project site; treat clause/table numbers
below as approximate pointers, not citations to a fixed page.

This note has two audiences: whoever rewrites `libarf`, and the
SAM3DBody-cpp maintainers, since `ARFWriter` is the thing that actually needs
to change to produce containers this project can read conformantly — see
[For SAM3DBody-cpp](#for-sam3dbody-cpp) at the end. It follows the same
"upstream issue" spirit as [`doc/UPSTREAM_ISSUE.md`](UPSTREAM_ISSUE.md).

**Status: Milestone 1 (numeric-id component graph), Milestone 2 (AAU
bitstream) and Milestone 3 (`LandmarkSet`/`AAU_LANDMARK`) are implemented.**
`structure` as Asset/LOD and the `id_map.txt` sidecar are done;
`samples/summerlove_0.arfz` has been regenerated across the milestones that
changed its shape -- all clean cutovers, the reader no longer accepts a
pre-rewrite shape at all. `LandmarkSet` has no real tracking data source in
this codebase, so it was verified with a synthetic round trip through the
Python bindings (`enable_landmarks()`/`append_landmark_frame()`) rather than
against a real sample -- see that section for details. Sections below are
marked `[done]` or `[pending]` accordingly. Still pending:
`TextureSet`/`TextureTarget`, `BlendshapeSet` shapes as GLB. The skin-weight
tensor stays sparse for now, a deliberate choice, not an oversight — see
that section.

**Bug found and fixed after Milestone 1 shipped:** the General Conventions
clause states plainly, "All references used in the ARF document are to the
id field of the referred item. Index-based referencing is not used in this
specification." Milestone 1's first pass got this backwards — it required
`id == array index` and rejected anything else, which would wrongly reject a
conformant container from another writer that numbers ids differently. Fixed
by resolving every JSON-level reference (`Node.parent`, `Skeleton.root`/
`joints`, `Skin.mesh`/`skeleton`, `BlendshapeSet.baseMesh`) against the
target's declared `id` field via a real lookup (`arfFindIdIndex()` in
`arf_reader.c`, a `Map` in `web/arf.js`), the same shape the pre-Milestone-1
string-id code already used, just keyed on a number instead of a string. The
writer is unaffected — it still assigns sequential `id == index`, which
remains a fully valid choice, just no longer a required one. AAU stream
`_index` fields (`aja_target_joint_index`, `afa_blendshape_index`) are
unaffected by this — they're positional into their own list by name and by
design (a real-time binary stream doing id lookups per joint would be an odd
choice), not document-level `id` references.

## Scope of this rewrite

**In scope** (the parts worth conforming to):

* `[done]` the numeric-id component graph (`Node`/`Skeleton`/`Skin`/`Mesh`/
  `BlendshapeSet`) that everything else sits on top of
* `[done]` the `structure` Asset/LOD model
* `[done]` the AAU bitstream (field widths, `AAU_LANDMARK`)
* `[done]` `LandmarkSet`
* `[pending]` `BlendshapeSet` shapes as bare-geometry GLB instead of raw delta tensors
* `[pending]` `TextureSet`/`TextureTarget` (parametric texture blending),
  declaration-only `AnimationLink` (no cross-framework retargeting runtime)
* `[done]` a non-normative `id_map.txt` sidecar for debugging (new idea, not
  in the spec — see [below](#id_maptxt-sidecar-done))

**Out of scope**, by explicit choice, revisit only if a real need shows up:

* ISOBMFF-based container (we only ever produce/consume the Zip-based one)
* RTP payload streaming
* protection/DRM (`ProtectionConfiguration`, encrypted data items)
* the `AnimationLink`/`Mapping`/`LinearAssociation` cross-framework
  retargeting *runtime* — we declare the object where required, we don't
  execute linear-association remapping between animation frameworks
* full Annex B `MPEG_node_avatar` glTF **scene** integration (embedding the
  avatar as a node in a larger glTF scene graph with externally-driven
  animation streams). We still adopt the numeric-id data model and the
  container-local `animations/*.bin` AAU streams, which the Zip container
  clause supports independently of scene embedding.

## Top-level document

`preamble` / `metadata` / `structure` / `components` / `data` as the five
top-level keys already matches. No change needed there.

## Component id model `[done]`

| | current (SAM3DBody-flavoured) | conformant target |
|---|---|---|
| object identity | string `id` (`"mesh0"`, `"skin0"`), joint identity *is* its name string | integer `id` on every component object; `name` is a separate, still-mandatory, human-readable string |
| cross-references | by matching string ids/names | by numeric id (or array index, depending on field — see per-component notes) |
| skeleton joints | `skeletons[0].joints` is the same string list as `components.nodes`' names, positionally | `Skeleton.joints` is an array of `Node` ids; `Skeleton.root` is a `Node` id, not a name |

This is the prerequisite everything else sits on. It touches `arf_json.c`
(schema walk), `arf_reader.c` (reference resolution), and `arf_writer.c`
(id assignment on write) regardless of which of the optional components below
get implemented.

## `structure` `[done]`

| | current | conformant target |
|---|---|---|
| shape | `structure.animationStreams: [{id, uri, frameworks}]` | `structure.assets: [Asset]`, each `Asset` has `name`, optional `isMain`, and `lods: [LOD]` |
| `LOD` | n/a | `name` plus arrays of numeric ids: `skins`, `meshes`, `skeletons`, `blendshapeSets`, `landmarkSets`, (`textureSets`) — which components belong to this asset/level of detail |
| animation stream location | declared directly in `arf.json` | **not** a field of the base document at all. In the glTF scene-integration path, `animationStreams` lives in the *scene's* `MPEG_node_avatar` extension, pointing at glTF accessors. Since we're not doing scene integration, our containers keep carrying `animations/*.bin` per the Zip-container clause, just without a JSON field naming them — a reader has to know to look in the `animations/` folder rather than being told the path by `arf.json`. |

Net effect: `structure.animationStreams` as a JSON field goes away; one
`Asset`/`LOD` entry replaces it for describing what's in the container, and
the animation `.bin` files become convention-located rather than
explicitly referenced.

## `Node` `[done]`

| field | status | notes |
|---|---|---|
| `id` | done | numeric, validated `== index` on read |
| `name` | done | |
| `mapping` | done, as an honest placeholder | this project has no verified taxonomy for the semantic path (that lives in the companion scene-description part, 23090-14), so a node's own name is emitted as a single-segment path rather than a fabricated one |
| `parent` | done | numeric, optional, absent only on the root |
| `children` | not emitted | optional in the spec and derivable from `parent`; skipped as redundant |
| `translation` / `rotation` / `scale` | done | each independently optional, default identity/zero/one |
| `transform` | done, reader only | a single 4x4 matrix, alternative to TRS, mutually exclusive with it. The reader decomposes a `transform` node to canonical TRS at load time (`arfDecomposeTransform` in `arf_reader.c`, mirrored in `web/arf.js`); the writer still only emits TRS, since nothing downstream of node loading consumes rest translation/rotation/scale at runtime anyway (`arfComposeGlobals` works from the per-frame baked AAU matrices only) |

**Open inconsistency worth flagging upstream to MPEG, not to SAM3DBody-cpp:**
the prose table for `Node` marks `transform` as conditionally-mandatory
(i.e. required only if TRS is absent) and `id` as mandatory, but the
machine-readable JSON Schema in the normative annex requires `transform`
unconditionally and doesn't list `id` as required at all. The prose reading
is the one to implement — it's internally consistent and matches how the
referenced glTF-based scene description (23090-14) treats TRS-vs-matrix — the
annex schema looks like it fell out of sync with the prose. Worth a short
issue against the spec itself once conformance work starts in earnest, the
same way `doc/UPSTREAM_ISSUE.md` is a short issue against SAM3DBody-cpp.

## `Skeleton` `[done]`

| field | status |
|---|---|
| `id`, `name` | done |
| `root` | done; numeric `Node` id, checked against the parentless node |
| `joints` | done; array of `Node` ids, checked to be `[0, 1, 2, ...]` in node order |
| `inverseBindMatrix` | done, singular field, one data reference for the whole `Nx16` dense tensor |
| `animationInfo` | not emitted — optional, skipped under the scope decision |

## `Skin` `[done]`

`components.skins` is now the object that ties a skeleton+mesh pairing
together, per spec, rather than a thin name-matching shim:

| field | status |
|---|---|
| `id`, `name` | done |
| `mapping` | done, as an honest placeholder (same caveat as `Node.mapping`) |
| `skeleton`, `mesh` | done — numeric ids, cross-checked to be `0` at load time (this library only ever holds one of each) |
| `blendshapeSet`, `landmarkSet`, `textureSet` | not added — optional in the spec, and `BlendshapeSet.baseMesh` already links the mesh to its blendshape set independently, so nothing currently needs these |
| `weights` | done as a numeric reference; **still the sparse tensor**, see [Tensor format](#tensor-format-no-sparse-in-the-spec) below |
| `proprietaryAnimations` | not added, optional, out of scope |

## `Mesh` `[done]`

| field | status |
|---|---|
| `id`, `name` | done |
| `path` | done, as an honest placeholder (same caveat as `Node.mapping`) |
| `data` | done as `[positions data id, indices data id]` — this project's own documented convention (`arf_format.h`), since the spec text available here doesn't prescribe the slot order beyond "mesh data" |

## `BlendshapeSet` `[id/name/baseMesh done, shapes pending]`

| field | status |
|---|---|
| `id`, `name`, `baseMesh` | done — numeric ids, `baseMesh` cross-checked to be `0` |
| `shapes` | **still pending.** Today: `shapes` is a one-element array naming a single data item that holds one dense tensor of raw per-vertex position deltas, `[n_shapes, n_verts, 3]` float32. **Spec**: an array of numeric data-item references, each pointing at its own GLB file containing *only* geometry (vertices + faces, no materials/textures) for that one shape. This is a bigger change than a field rename — it means encoding/decoding minimal GLB (JSON chunk + BIN chunk, accessors, bufferViews), one file per blendshape target, and building the delta against the base mesh rather than storing a pre-computed delta blob. |
| `animationInfo` | not added, optional, skipped under scope decision |

## `LandmarkSet` `[done]`

Implemented: `struct arfLandmarks` in `arf.h` (`numberOfLandmarks` +
`vertexIndex`, mesh-vertex indices), `components.landmarkSets[0]` with
`name`/`id`/`baseMesh`/`vertices` (a dense `[n]` uint32 tensor, cross-checked
against the mesh vertex count on load), and the write API
`arfEnableLandmarks()`/`arfAppendLandmarkFrame()` mirroring
`arfEnableFace()`/`arfAppendFaceFrame()`. `animationInfo` not added, optional,
skipped under the same scope decision as elsewhere.

No producer in this codebase tracks real landmark data, so this was verified
with a synthetic avatar built through the Python bindings: `enable_landmarks`
+ `append_landmark_frame`, saved, reloaded, and diffed field-for-field
(vertex indices, per-frame xyz positions) against what was written — plus a
combined face+landmark container to exercise the `data[]`/`components`
`isLast`/ordering logic when every optional track is present at once.
`web/arf.js` is unaffected by design: it only ever opens `animations/
joints.bin`, the same way it already never opens `animations/face.bin`, so
it has no landmark-shaped hole to fall into.

## AAU_LANDMARK `[done]`

Implemented per Table 42: `ala_landmark_set_id` (16-bit, checked against
`landmarkSets[0].id`), a one-byte `velocity_present`/`confidence_present`/
`is_3d_flag`/reserved flag field, `landmark_count_minus1`, then per-entry
`landmark_index` + 2 or 3 coordinate floats + optional velocity/confidence
(parsed and discarded, same as `AAU_JOINT`/`AAU_BLENDSHAPE`). Landmarks are
always stored internally as 3 floats (z=0 for a 2D frame) so the in-memory
shape stays uniform regardless of what a source stream sends; the writer
always emits `is_3d_flag=1` since that's the only shape it has to give.
Lives in its own `animations/landmarks.bin`, the same one-stream-per-modality
pattern as `joints.bin`/`face.bin`, with its own profile string,
`"arf-landmark-v1"` — a name this project introduces, following the `arf-
body-v1`/`arf-face-v1` pattern, since there was no established one to
inherit here.

## `TextureSet` / `TextureTarget` — entirely new

Not implemented at all today; this is the "glTF scenes and textures" item
that's explicitly in scope. `TextureSet` names a material (a numeric
data-item reference plus a `materialPath` locating the actual texture inside
that item — most likely a path into a glTF/GLB material), and a list of
`TextureTarget`s, each of which is itself a reference to a texture plus a
locating path. `animationInfo` is **mandatory** here (unlike everywhere else
it's optional), but per the scope decision this can be a minimal declaration
rather than a functioning cross-framework mapping.

Practically, this is the most expensive in-scope item: it needs actual image
decoding (PNG/JPEG — a new dependency this project doesn't currently have),
a texture-blend shader path in `arf_render.c`, and a GLB/material reader to
resolve `materialPath`/`texturePath`. It shares the GLB-parsing primitive
that `BlendshapeSet` needs, so building that primitive once and reusing it
for both is the efficient order of work.

## `Data` `[done]`

| field | status |
|---|---|
| `id` | done — numeric, this project's own sequential convention (`ARF_DATA_ID_*` in `arf_format.h`) |
| `name`, `uri`, `byteLength` | done, unchanged in shape |
| `type` | done — renamed from the old `mimeType` |
| MIME values | unchanged: `application/mpeg.arf.dense` / `application/mpeg.arf.sparse` (the second one is invented, see below). Spec only defines `application/mpeg.arf.dense`. |
| `offset` | not added, optional |
| `compression` | not added, optional |
| `protection` | not added, optional, out of scope |

## Tensor format: no sparse in the spec `[decision made: staying sparse]`

The only tensor layout the spec defines is dense: `num_of_dims`, `dims[]`,
`dtype` (a glTF component-type code — this part already matches what
`arf_format.h` does), then raw row-major `data`. There is no sparse tensor
format anywhere in the document.

`skin_weights.bin`'s sparse encoding (index/value pairs, `application/
mpeg.arf.sparse`) is therefore not a deviation from one spec detail — it's
an entire representation the spec doesn't define. Going conformant here
means a real choice, not a wire-format fix:

1. **Emit dense weight tensors.** Correct per spec, but the size cost is
   real: the sample container's skin weights go from 51,337 sparse nonzeros
   to 18,439 x 127 = 2,341,373 floats, roughly an 18x increase for that one
   file.
2. **Keep the sparse encoding as a documented, deliberate deviation**, the
   same way this project already documents its other SAM3DBody-flavoured
   choices, and accept that this one specific file in a conformant container
   won't validate against Annex E.

**Decided:** option 2, keep the sparse encoding. Milestone 1 only changed how
`Skin.weights` is *referenced* (a numeric data id instead of a string one);
the tensor bytes themselves are untouched. Revisit if a real interop need
against another conformant reader ever comes up.

## AAU bitstream `[done, except AAU_LANDMARK]`

**Correction to what this section originally said:** it claimed
`aau_unit_type` being 7 bits + 1 reserved bit, packed into what was already a
byte-aligned `uint8`, was "bit-compatible in effect." That was wrong on a
closer look: `(unit_type << 1) | reserved` is not the same byte as a raw
`unit_type` value (type 2 packs to byte `4`, not `2`). It also missed a
bigger issue entirely: the spec's bitstream tables use `uimsbf` throughout
(unsigned integer, most significant bit first — the same notation ISOBMFF/
MPEG-2 Systems use, where it means big-endian), while every field in this
format was little-endian. Both are now fixed — see
`arfCursorReadU16BE`/`arfBufferWriteU32BE`/etc. in `arf_bytes.h`, used only
by the AAU stream; tensors stay little-endian, since Annex E's tensor tables
don't use bitstream notation at all. The per-type payload differences:

| | old | conformant (done) |
|---|---|---|
| `AAU_BLENDSHAPE` target reference | a UTF-8 string (`target_blendshape_set_id`) | a big-endian 16-bit numeric id, checked against `blendshapeSets[0].id` |
| `AAU_JOINT` target reference | none | a big-endian 16-bit `joint_set_id`, checked against `skeletons[0].id` (new field, didn't exist before) |
| `AAU_BLENDSHAPE` / `AAU_JOINT` counts | 32-bit little-endian raw count | big-endian 16-bit **count-minus-1** |
| `AAU_BLENDSHAPE` / `AAU_JOINT` indices | 32-bit little-endian | big-endian 16-bit |
| `AAU_JOINT` velocity | didn't exist | optional per-joint big-endian float32[16], flagged by a presence bit packed with 7 reserved bits into one byte; read and discarded, since nothing here consumes it yet |
| `AAU_BLENDSHAPE` confidence | a `uint8` flag, always 0 | a single presence *bit* (packed with 7 reserved bits into the same byte), plus an optional per-entry confidence float when set; read and discarded, same as velocity |
| `AAU_CONFIG` profile string | 32-bit little-endian length prefix (this format's general string convention) | **8-bit** length prefix — the spec gives this one field its own narrower encoding, distinct from every other string in the format (there are no others left after the two rows above) |

**Still not implemented:** `AAU_LANDMARK` (type 3) and the `LandmarkSet`
component it depends on. The type code is recognized
(`ARF_AAU_LANDMARK` in `arf_format.h`) so a landmark unit in an incoming
stream is still skipped safely by the generic unrecognized-type path: there
is nothing here to write one, and nothing to test a reader against without a
`LandmarkSet` to cross-check it against, so implementing its parsing now
would be speculative. Revisit alongside `LandmarkSet` itself.

This touched `arf_bytes.h` (new BE primitives + 8-bit string, old 32-bit
string helpers removed as they became fully unused), `arf_reader.c`/
`arf_writer.c`'s AAU encode/decode paths, and `web/arf.js`'s mirror.
`tools/mutate_arf.py` needed two small fixes of its own: it hardcoded
little-endian when reading/writing the AAU stream's `unit_length` for two of
its synthetic mutations, which is exactly the kind of collateral damage a
byte-order change causes in anything else that speaks the wire format.

## `id_map.txt` sidecar `[done]`

Implemented as designed: `arfWriteIdMap` in `arf_writer.c` emits a flat
`<type>\t<id>\t<name>` line per node, plus one line each for the mesh/skin/
skeleton/blendshapeSet, added to the archive as a plain non-normative entry
never referenced from `data[]`/`structure`/`components`. Documented in
`doc/ARF.md`'s own `id_map.txt` section.

## For SAM3DBody-cpp

`ARFWriter` is the thing that actually produces `.arfz` containers, so
everything above is only real once it emits the conformant shapes.
`ARFPlayer`'s side is now done for both the numeric-id rewrite and the AAU
bitstream (both were clean cutovers — `libarf` no longer reads either
pre-rewrite shape at all), so `ARFWriter` needs, concretely:

* switch every component (`nodes`, `skeletons`, `skins`, `meshes`,
  `blendshapeSets`) from string ids to numeric ids, keeping `name` as a
  separate field. Ids do **not** need to equal array position — the spec
  resolves every reference by id, not index (see the bug note above) — but
  sequential `id == index` is a perfectly valid choice and the one
  `ARFPlayer` itself makes, so it's the path of least surprise if `ARFWriter`
  has no reason to do otherwise
* replace `structure.animationStreams` with `structure.assets[].lods[]`, and
  drop any field naming the animation stream location — `animations/
  joints.bin` / `animations/face.bin` are convention-located now, not
  declared in `arf.json`
* rename `data[].mimeType` to `data[].type`, and give every `data[]` entry a
  numeric `id` alongside its existing `name`
* `Skeleton.inverseBindMatrices` (plural) becomes `inverseBindMatrix`
  (singular) — still one data item for the whole `Nx16` tensor
* `Mesh.positions`/`Mesh.indices` become `Mesh.data: [positionsId, indicesId]`
  — this exact slot order (positions first) is `ARFPlayer`'s own convention,
  worth confirming rather than assuming if `ARFWriter` picks its own order
  independently
* skin weights can stay on the sparse encoding — that's a joint decision
  already made on this side, not something `ARFWriter` needs to change
* rewrite the `AAU_CONFIG`/`AAU_BLENDSHAPE`/`AAU_JOINT` encoders to
  **big-endian**, the field widths and count-minus-1 convention, the new
  `aja_joint_set_id`/`afa_blendshape_set_id` fields (write the same id as the
  corresponding `skeletons[0].id`/`blendshapeSets[0].id`), and the 8-bit
  profile-string length — see [AAU bitstream](#aau-bitstream-done) for the
  exact byte layout
* if landmark tracking is ever added, `components.landmarkSets` +
  `AAU_LANDMARK` are both implemented and waiting on this side — `name`/
  `id`/`baseMesh`/`vertices` (a dense uint32 tensor of mesh-vertex indices),
  its own `animations/landmarks.bin` with profile `"arf-landmark-v1"`, and
  the `ala_*` field layout in [AAU_LANDMARK](#aau_landmark-done)

Still open, not yet needed for the two sides to agree on today's shape:

* if blendshape/facial tracking export is ever extended, emit each shape as
  its own geometry-only GLB rather than one combined delta tensor — pending
  on `ARFPlayer`'s side too

Recorded here so both sides can move in lockstep instead of `ARFPlayer`
conforming to a spec that `ARFWriter` no longer produces containers matching.
