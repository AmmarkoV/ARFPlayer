# Conformance gaps: SAM3DBody-flavoured ARF vs. ISO/IEC 23090-39

This is the scoping note for turning `libarf` from a reader/writer of
SAM3DBody-flavoured ARF into a reader/writer of conformant ARF. It exists so
the rewrite has a concrete target to diff against, and so the differences are
written down once rather than rediscovered file by file.

Field names, types, and mandatory/optional status below are facts about the
format and are stated plainly. Descriptions are paraphrased in this project's
own words. No schema or bitstream-syntax text is quoted from ISO/IEC 23090-39;
consistent with [`doc/ARF.md`](ARF.md), no specification text is reproduced
here. The source used to compile this note is the draft text currently
published at the MPEG ARF project site (its own title page marks it "CD
stage," an earlier stage than this project previously assumed here); treat
clause/table numbers below as approximate pointers, not citations to a fixed
page, and expect fields to keep moving until the text reaches FDIS.

This note has two audiences: whoever rewrites `libarf`, and the
SAM3DBody-cpp maintainers, since `ARFWriter` is the thing that actually needs
to change to produce containers this project can read conformantly — see
[For SAM3DBody-cpp](#for-sam3dbody-cpp) at the end. It follows the same
"upstream issue" spirit as [`doc/UPSTREAM_ISSUE.md`](UPSTREAM_ISSUE.md).

**Status: every item originally scoped in is now implemented** --
Milestone 1 (numeric-id component graph), Milestone 2 (AAU bitstream),
Milestone 3 (`LandmarkSet`/`AAU_LANDMARK`), Milestone 4
(`TextureSet`/`TextureTarget`) and Milestone 5 (`BlendshapeSet.shapes` as
per-shape GLB). `structure` as Asset/LOD and the `id_map.txt` sidecar are
done; `samples/summerlove_0.arfz` has been regenerated across the
milestones that changed its shape -- all clean cutovers, the reader no
longer accepts a pre-rewrite shape at all. `LandmarkSet` and `TextureSet`
have no real data source in this codebase (no landmark tracking, no
textured avatars), so both were verified with synthetic round trips
through the Python bindings rather than against a real sample -- see their
sections for details. Sections below are marked `[done]` throughout now.
A later re-read against the spec text found two more gaps outside any of
the milestones above -- `preamble.supportedAnimations`'s shape and
`metadata.age`/`gender` being missing altogether -- both now fixed, see
[`Preamble`/`Metadata`](#preamble--metadata-done). The two remaining,
deliberate deviations from the spec are the sparse skin-weight tensor (the
spec only defines a dense one) and centimetres as the unit (the spec names
the metre as its default) -- both documented choices, not oversights, see
their sections for why they stay that way.

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

**Two more gaps found on a later re-read against the spec text, both now
fixed:** the "everything scoped in is implemented" status above turned out to
still have two undocumented misses in `preamble`/`metadata`, neither flagged
by the milestones above because neither was in scope at the time — see
[`Preamble`/`Metadata`](#preamble--metadata-done) below for what changed and
why. Re-reading against the spec also surfaced (but did not change) that this
project's centimetre convention conflicts with the General Conventions
clause's stated default unit (the metre) — see
[Units: centimetres, not metres](#units-centimetres-not-metres-deliberate-deviation)
below.

## Scope of this rewrite

**In scope** (the parts worth conforming to):

* `[done]` the numeric-id component graph (`Node`/`Skeleton`/`Skin`/`Mesh`/
  `BlendshapeSet`) that everything else sits on top of
* `[done]` the `structure` Asset/LOD model
* `[done]` the AAU bitstream (field widths, `AAU_LANDMARK`)
* `[done]` `LandmarkSet`
* `[done]` `BlendshapeSet` shapes as bare-geometry GLB instead of raw delta tensors
* `[done]` `TextureSet`/`TextureTarget`, as a static asset declaration (no
  AAU counterpart exists to animate it), declaration-only `AnimationLink`
  (no cross-framework retargeting runtime)
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
top-level keys matches. Two field-level gaps inside those two objects were
missed by the milestones above (neither `preamble` nor `metadata` had its own
scoped milestone) and are fixed now — see immediately below.

## `Preamble` / `Metadata` `[done]`

| field | status |
|---|---|
| `preamble.signature`, `preamble.version` | done, unchanged |
| `preamble.supportedAnimations` | **was the wrong shape**, now fixed |
| `metadata.name`, `metadata.id` | done, unchanged |
| `metadata.age`, `metadata.gender` | **were missing entirely**, now added |

`supportedAnimations` is a `SupportedAnimations` object in the schema —
`bodyAnimations`/`faceAnimations`/`handAnimations`/`landmarkAnimations`/
`proprietaryAnimations`, each an array of strings, none of them individually
required — not the flat array of profile-name strings
(`["arf-body-v1", "arf-face-v1", ...]`) this library used to write. Fixed in
`arfWriteJson()` (`arf_writer.c`): each modality this avatar actually carries
gets its own one-entry array (`bodyAnimations` always, `faceAnimations`/
`landmarkAnimations` only when `hasFace`/`hasLandmarks`). The spec suggests
each entry "should be formatted as a URN that includes an identifier of the
framework..." but never actually defines a concrete URN scheme in the text
available here, so this library keeps its own plain `arf-body-v1`/
`arf-face-v1`/`arf-landmark-v1` profile names — the same strings
`AAU_CONFIG.profile` already carries on the wire — rather than fabricate a
URN grammar with nothing to base it on. Nothing parses this field back on
read (`arf_reader.c`/`web/arf.js` never touched it before or after this fix),
so this was a write-side-only defect: every container this library wrote had
a `preamble.supportedAnimations` that would fail schema validation as a type
mismatch (array where an object is required), even though this library's own
reader never noticed.

`metadata.age`/`metadata.gender` are mandatory (Metadata schema:
`required: ["name", "id", "age", "gender"]`) and were absent from the data
model entirely — not merely unfilled-in, `struct arfAvatar` (`arf.h`) had no
field for either. Added: `int age` and `char gender[ARF_MAX_NAME]` alongside
`name`/`id`, written and parsed in `arf_writer.c`/`arf_reader.c`, mirrored in
`web/arf.js` and both bindings (the Python `ctypes` struct is a manual
memory-layout mirror of `struct arfAvatar` — inserting fields there without
updating `bindings/python/arf.py` in lockstep would have silently corrupted
every field read after them). Like `Node.mapping`/`Mesh.path`, this library
has no real source for either value (no field in the producing pipeline
carries a tracked person's age or gender), so both are honest, documented
placeholders rather than fabricated data: `age = -1` ("unknown", chosen over
`0` specifically so it can't be misread as an age of zero), `gender =
"unspecified"`. A real source, if one is ever wired up, would set these the
same way `name`/`id` are already set today, by direct field assignment before
`arfSave()` — there is no dedicated setter function for `name`/`id` either.

## Units: centimetres, not metres `[decision made: staying centimetres]`

The General Conventions clause states plainly that ARF "adopts the metre as
the default unit of measurement." This library has used centimetres
throughout since before the numeric-id rewrite — inherited unchanged from the
original SAM3DBody-flavoured design — and nothing downstream agrees with the
spec's default either: the producing pipeline's own tracker output is in
centimetres, and nothing in this codebase (or, as far as this project has
verified, in SAM3DBody-cpp) converts at any boundary.

This was never flagged as a deviation anywhere in this project's own
documentation prior to this re-read — every other SAM3DBody-flavoured choice
this project keeps (the sparse skin-weight tensor, `Node.mapping`'s
placeholder value, `TextureSet.materialPath`) is called out explicitly as a
deliberate, documented deviation; units were the one thing stated as fact
(`doc/ARF.md`'s "Units are centimetres") with no acknowledgment it disagrees
with the spec at all.

**Decided:** stay in centimetres, the same way the sparse skin-weight tensor
stays sparse — converting at the read/write boundary is possible (scale every
length-valued field by 0.01 on write, 100 on read) but nothing internal to
this library needs it, and it would only matter the moment a real conformant
reader or writer that assumes metres enters the picture. Revisit if that
interop need ever comes up, the same trigger condition as the tensor
decision above.

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

## `BlendshapeSet` `[done]`

| field | status |
|---|---|
| `id`, `name`, `baseMesh` | done — numeric ids, `baseMesh` cross-checked to be `0` |
| `shapes` | done — array of numeric data-item ids, one GLB file per blendshape target |
| `animationInfo` | not added, optional, skipped under scope decision |

`shapes[i]` used to name a single data item holding one dense tensor of raw
per-vertex position deltas for every shape at once, `[n_shapes, n_verts, 3]`
float32 — this project's own convention, not the spec. Now each `shapes[i]`
is its own data item, a minimal GLB (binary glTF) file with only geometry
(one `POSITION` accessor, one indices accessor, no materials or textures),
whose topology must match `baseMesh` exactly — both requirements straight
from the spec text.

**The harder part wasn't the file format, it was the semantics.** A shape's
GLB stores its **absolute** deformed vertex positions, not a delta: the
spec's blend formula is `v_out = v_0 + sum_i(w_i * (v_i - v_0))`, i.e. `v_i`
(what's stored) minus `v_0` (the base mesh) is the delta, computed at blend
time — it is not what gets written to disk. `libarf`'s runtime keeps working
in deltas internally regardless (`arfBlendshapes.deltas`,
`arfApplyBlendshapes()` — both completely unchanged), so the
absolute-vs-delta conversion happens only at the read/write boundary: the
writer adds the base mesh back once per shape before encoding its GLB, the
reader subtracts it once after decoding. No public API changed —
`arfEnableFace()`/`arfAppendFaceFrame()` still take deltas, exactly as
before this milestone.

That conversion has a real, if usually small, precision cost: `(base +
delta) - base` is not always exactly `delta` in float32 — a shape's stored
absolute position sits at the base mesh's coordinate magnitude
(centimetres, order 10^2), while a fine facial delta can be two or three
orders of magnitude smaller, so the round trip can lose a meaningful
fraction of that delta's precision to rounding. Measured on a synthetic
test: deltas around 0.05–0.2 round-tripped to within ~4e-7 absolute error —
negligible here, but this is a genuine property of the spec's storage
choice, not a bug to route around, and is documented as such in
`arf_format.h`/`doc/ARF.md`.

Implemented in a new, self-contained module, `src/libarf/arf_glb.{c,h}`
(~250 lines): `arfGlbWriteMesh()`/`arfGlbReadMesh()`, a minimal binary glTF
encoder/decoder — one mesh, one primitive, `POSITION` + indices, no
materials or textures, not a general-purpose glTF library. It reuses this
project's existing JSON parser (`arf_json.c`) for the GLB's embedded JSON
chunk rather than writing a second one, and the existing little-endian
cursor/buffer helpers in `arf_bytes.h` (GLB is little-endian throughout,
like every other payload here except the AAU stream). The reader rejects
anything with a different shape (multiple buffers, unexpected component
types, a missing BIN chunk) rather than guessing, and additionally checks a
shape's indices are byte-identical to the base mesh's, not just
equal-count, since the spec requires identical topology.

Since `shapes` is now variable in count (as many GLBs as there are
blendshape targets — the real pipeline's own face export uses 72), and
`TextureSet`'s targets already made data ids variable in count too, the
writer's `data[].id` assignment was generalized from individual fixed
constants to a single running counter computed once per save (`struct
arfDataIdPlan` in `arf_writer.c`): `mesh_positions`/`mesh_indices`/
`skin_weights`/`inverseBind` stay fixed at `0`–`3` (always exactly one
instance, in that order), and every optional/variable component after them
— blendshape shapes, landmark vertices, the texture material and its
targets — gets the next id in whatever order it's actually present, rather
than a value baked in at compile time. This is transparent to readers: they
already resolved every reference by id lookup (a real search over `data[]`),
never by treating an id as a direct index — see the id/index bug note near
the top of this document.

Verified with a synthetic round trip through the Python bindings (the
`enable_face`/`append_face_frame` API is completely unchanged): a small quad
mesh (4 vertices, 2 triangles — enough to exercise real indexing, unlike a
single-triangle toy case) with two blendshape targets, saved and reloaded,
deltas compared against the originals within float32 tolerance. Each
resulting `.glb` was additionally parsed with fresh, independent Python code
(not this project's own `arf_glb.c`) to confirm it is genuinely valid binary
glTF — correct header, correct chunk framing, a standard accessor/
bufferView/mesh JSON structure any glTF viewer would recognize — not just
bytes this library alone can make sense of.

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

## `TextureSet` / `TextureTarget` `[done]`

**Re-scoped on inspection, in a good way.** The original scoping note here
assumed this would need image decoding (PNG/JPEG), a texture-blend shader
path, and a GLB/material reader — the most expensive item in the whole
rewrite. On actually implementing it, two things simplified that a lot:

1. **There is no AAU unit for texture blend weights.** The Animation Stream
   Format clause defines exactly three sample formats — facial (blendshape),
   joint, landmark — and none of them is texture. `TextureSet` is therefore
   a **static asset declaration**, like `Mesh`/`Skin`, not an animated track
   like `BlendshapeSet`/`LandmarkSet`. No new AAU type, no runtime blending
   logic, no shader work.
2. **`libarf` never needs to decode the images it carries.** `material`/
   `texture` reference `Data` items exactly like every other tensor
   reference in this format — the difference is only that their content is
   opaque image bytes instead of a dense/sparse tensor. `libarf` already
   treats `mesh_positions.bin` as "bytes with a declared shape," not
   semantically as geometry; texture bytes get the identical treatment.
   Decoding pixels is a consumer's job (a renderer), not the container
   library's — consistent with this project's "depends on nothing but
   libzip and libm" design, which a PNG/JPEG dependency would have broken.

Implemented: `struct arfTextureSet`/`struct arfTextureTarget` in `arf.h`
(name + MIME type + opaque byte blob, one material and N targets),
`components.textureSets[0]` with `name`/`id`/`animationInfo`/`material`/
`materialPath`/`targets[]`, `skins[0].textureSet` (the only link tying a
`TextureSet` to anything, since it has no `baseMesh`/mesh field of its own,
unlike `BlendshapeSet`/`LandmarkSet`), and the write API
`arfEnableTextureSet()`/`arfAddTextureTarget()`. `data[]` ids for the
material/targets are the first variable-count case in this format (id 6 for
the material, 7, 8, ... for each target — every other optional component
before this one added exactly one fixed data item).

Two conventions invented here, undocumented by the spec text available:
`animationInfo: []` (mandatory in the spec, but no `AnimationLink` enum
value means "texture," so an honestly empty array beats a fabricated link),
and `materialPath`/`texturePath: ""` (they "indicate where the texture can
be found in the item," for formats where one data item embeds several
textures — since every data item here is one flat image, there is nothing
to locate within it).

Verified with a synthetic round trip through the Python bindings: a real,
valid, freshly-generated PNG (built with nothing but `zlib`/`struct`, no
image library) as the material, plus two more as texture targets, saved and
reloaded, comparing every byte against the originals — confirms the
container carries arbitrary opaque binary content faithfully end to end
without `libarf` ever parsing PNG.

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
* `BlendshapeSet.shapes[i]` becomes one minimal GLB file per blendshape
  target (geometry only, no materials/textures, topology identical to the
  base mesh) instead of one combined dense delta tensor — and each GLB's
  `POSITION` accessor must hold that shape's **absolute** vertex positions
  (base mesh + delta), not the delta itself; the blend formula subtracts the
  base mesh back out at blend time. See
  [BlendshapeSet](#blendshapeset-done) for the exact reasoning and the
  float32-precision note; `arf_glb.c` is a small enough reference encoder/
  decoder to build a compatible writer from directly if useful
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
* if textured avatar export is ever added, `components.textureSets` is
  implemented and waiting too — no AAU stream involved, since there is no
  animated track for texture blend weights at all. `material`/`targets[].
  texture` reference plain image `data[]` items (real MIME type, e.g.
  `image/png`); `skins[0].textureSet` is the only link to add on the
  `Skin` side. See [TextureSet/TextureTarget](#textureset--texturetarget-done)
  for the exact JSON shape and the `animationInfo`/`materialPath` conventions
* turn `preamble.supportedAnimations` from a flat array into the
  `SupportedAnimations` object (`bodyAnimations`/`faceAnimations`/
  `landmarkAnimations`, each an array of profile strings), and add
  `metadata.age`/`metadata.gender` (mandatory, an integer and a string
  respectively) — see [`Preamble`/`Metadata`](#preamble--metadata-done) for
  the exact shape and this project's own placeholder values for the fields
  it has no real source for

Nothing is left "still open" on `ARFPlayer`'s side at this point — every
item above is implemented and waiting for `ARFWriter` to match it.

Recorded here so both sides can move in lockstep instead of `ARFPlayer`
conforming to a spec that `ARFWriter` no longer produces containers matching.
