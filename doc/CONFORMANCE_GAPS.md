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

**Status: Milestone 1 is implemented** — the numeric-id component graph,
`structure` as Asset/LOD, and the `id_map.txt` sidecar are done, and
`samples/summerlove_0.arfz` has been regenerated in the new shape (this was
a clean cutover: the reader no longer accepts the old string-id shape at
all). Sections below are marked `[done]` or `[pending]` accordingly. Still
pending: the AAU bitstream, `LandmarkSet`, `TextureSet`/`TextureTarget`,
`BlendshapeSet` shapes as GLB. The skin-weight tensor stays sparse for now,
a deliberate choice, not an oversight — see that section.

## Scope of this rewrite

**In scope** (the parts worth conforming to):

* the numeric-id component graph (`Node`/`Skeleton`/`Skin`/`Mesh`/
  `BlendshapeSet`) that everything else sits on top of
* the `structure` Asset/LOD model
* the AAU bitstream (field widths, `AAU_LANDMARK`)
* `LandmarkSet`
* `BlendshapeSet` shapes as bare-geometry GLB instead of raw delta tensors
* `TextureSet`/`TextureTarget` (parametric texture blending), declaration-only
  `AnimationLink` (no cross-framework retargeting runtime)
* a non-normative `id_map.txt` sidecar for debugging (new idea, not in the
  spec — see [below](#id_maptxt-sidecar-done))

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

## `LandmarkSet` — entirely new

Not implemented at all today. Needs: `name`, `id`, `baseMesh` (a mesh
reference), `vertices` (a data-item reference to the list of vertex indices
making up the landmark set), optional `animationInfo`. Paired with the new
`AAU_LANDMARK` unit type below.

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

## AAU bitstream

`aau_unit_type` being 7 bits + 1 reserved bit, packed into what's already a
byte-aligned `uint8` in `arf_format.h`, turns out to be bit-compatible in
effect — that part of the original worry in `doc/ARF.md` isn't actually a
problem. The real differences are in the per-type payloads:

| | current | conformant target |
|---|---|---|
| `AAU_BLENDSHAPE` target reference | a UTF-8 string (`target_blendshape_set_id`) | a 16-bit numeric id |
| `AAU_BLENDSHAPE` / `AAU_JOINT` counts | 32-bit raw count | 16-bit **count-minus-1** |
| `AAU_BLENDSHAPE` / `AAU_JOINT` indices | 32-bit | 16-bit |
| `AAU_JOINT` | no set-id field, no velocity | has a 16-bit joint-set id field the current format is missing entirely, plus an optional per-joint velocity block (flagged by a presence bit) |
| `AAU_BLENDSHAPE` confidence | a `uint8` flag, always 0 | a single presence *bit* (packed with 7 reserved bits into the same byte), plus an optional per-entry confidence float when set |
| `AAU_LANDMARK` | doesn't exist | new unit type: a 16-bit landmark-set id, presence bits for velocity/confidence, a 2D-vs-3D flag, 16-bit count-minus-1, then per-entry index + 2 or 3 floats of position + optional velocity/confidence |

This is a self-contained rewrite of the AAU encode/decode paths in
`arf_reader.c`/`arf_writer.c` plus `arf_format.h`, and needs mirroring in
`web/arf.js` same as any other format change.

## `id_map.txt` sidecar `[done]`

Implemented as designed: `arfWriteIdMap` in `arf_writer.c` emits a flat
`<type>\t<id>\t<name>` line per node, plus one line each for the mesh/skin/
skeleton/blendshapeSet, added to the archive as a plain non-normative entry
never referenced from `data[]`/`structure`/`components`. Documented in
`doc/ARF.md`'s own `id_map.txt` section.

## For SAM3DBody-cpp

`ARFWriter` is the thing that actually produces `.arfz` containers, so
everything above is only real once it emits the conformant shapes.
`ARFPlayer`'s side of the numeric-id rewrite is now done (this was a clean
cutover — `libarf` no longer reads the old string-id shape at all), so
`ARFWriter` needs, concretely:

* switch every component (`nodes`, `skeletons`, `skins`, `meshes`,
  `blendshapeSets`) from string ids to numeric ids, keeping `name` as a
  separate field — every component's id equals its index in its own
  `components.<array>`
* replace `structure.animationStreams` with `structure.assets[].lods[]`, and
  drop any field naming the animation stream location — `animations/
  joints.bin` / `animations/face.bin` are convention-located now, not
  declared in `arf.json`
* rename `data[].mimeType` to `data[].type`, and give every `data[]` entry a
  numeric `id` (sequential) alongside its existing `name`
* `Skeleton.inverseBindMatrices` (plural) becomes `inverseBindMatrix`
  (singular) — still one data item for the whole `Nx16` tensor
* `Mesh.positions`/`Mesh.indices` become `Mesh.data: [positionsId, indicesId]`
  — this exact slot order (positions first) is `ARFPlayer`'s own convention,
  worth confirming rather than assuming if `ARFWriter` picks its own order
  independently
* skin weights can stay on the sparse encoding — that's a joint decision
  already made on this side, not something `ARFWriter` needs to change

Still open, not yet needed for the two sides to agree on today's shape:

* rewrite the `AAU_BLENDSHAPE`/`AAU_JOINT` payload encoders to the field
  widths and count-minus-1 convention in [AAU bitstream](#aau-bitstream), and
  add the `aja_joint_set_id` field — pending on `ARFPlayer`'s side too
* if blendshape/facial tracking export is ever extended, emit each shape as
  its own geometry-only GLB rather than one combined delta tensor — pending
  on `ARFPlayer`'s side too

Recorded here so both sides can move in lockstep instead of `ARFPlayer`
conforming to a spec that `ARFWriter` no longer produces containers matching.
