# The ARF container, from a reader's point of view

This is the specification `libarf` implements. It describes the bytes a
`.arfz` container actually contains and what a reader has to do with them.

It is adapted from `knowledge/ARF.md` in
[SAM3DBody-cpp](https://github.com/AmmarkoV/SAM3DBody-cpp), which documents the
same bytes from the writer's side. Where that document explains *why the writer
chose* something, this one explains *what a reader must handle*.

The executable version of this document is
[`src/libarf/arf_format.h`](../src/libarf/arf_format.h) — every constant and
layout below appears there, and that file is the one to diff when a container
stops loading.

## Conformance status

ARF is ISO/IEC 23090-39 (MPEG-I Part 39). `libarf`'s `arf.json` component
graph — numeric ids resolved by matching value, not array position;
`structure` as Asset/LOD; `LandmarkSet`; `TextureSet`/`TextureTarget`;
`BlendshapeSet.shapes` as per-shape GLB targets — and the AAU animation-stream
bitstream (field widths, big-endian byte order, `AAU_LANDMARK`) have been
checked against the FDIS-stage text and match it; see
[`doc/CONFORMANCE_GAPS.md`](CONFORMANCE_GAPS.md) for the full accounting of
what was changed and why. Still outstanding, and still this project's own
convention: the sparse skin-weight tensor (the spec only defines a dense
one) — a deliberate, documented choice, not an oversight.

This container format traces back to
[SAM3DBody-cpp](https://github.com/AmmarkoV/SAM3DBody-cpp)'s `ARFWriter`,
which was originally designed against the published system-level overview
article rather than the bitstream-syntax text:

> J. Regateiro, A. Trioux, Q. Avril, "The MPEG Avatar Representation Format
> (ARF): An Interoperable Container and Animation Framework for Avatars,"
> *IEEE Computer Graphics and Applications*, 2026.
> <https://ieeexplore.ieee.org/document/11667221>

`tools/validate_arf.py`, copied here unchanged from that project, validates
containers in the *original* SAM3DBody-flavoured shape (string component ids,
`structure.animationStreams`) and is no longer a match for what `libarf`
itself reads and writes as of the numeric-id rewrite — it stays useful as a
record of what the upstream writer emits today, not as this library's own
oracle. `doc/CONFORMANCE_GAPS.md` lists what `ARFWriter` would need to change
to produce containers this library reads conformantly.

No specification text or figures are reproduced here. ISO and IEEE retain
copyright in their own publications regardless of this project's license.

## Container

A `.arfz` is a plain ZIP. Integers and floats in the dense/sparse tensor
payloads are little-endian, this project's own convention. The AAU animation
stream is the one exception: its bitstream tables use `uimsbf` (unsigned
integer, most significant bit first), the same MPEG-systems convention
ISOBMFF/MPEG-2 Systems use, where it means big-endian — see
"Avatar Animation Units" below.

```
<stem>_<id>.arfz
├── arf.json                    the base avatar model
├── id_map.txt                  non-normative id -> name debug index, see below
├── data/
│   ├── mesh_positions.bin      dense  [n_verts, 3]            float32
│   ├── mesh_indices.bin        dense  [n_tris, 3]             uint32
│   ├── skin_weights.bin        sparse dims [n_verts, n_joints]
│   ├── inv_bind_pose.bin       dense  [n_joints, 16]          float32
│   ├── face_blendshape_<i>.glb GLB, one per blendshape target          (optional)
│   ├── landmark_vertices.bin   dense  [n_landmarks]           uint32   (optional)
│   ├── texture_material.bin    opaque image bytes                     (optional)
│   └── texture_target_<i>.bin  opaque image bytes, one per target     (optional)
└── animations/
    ├── joints.bin              AAU_CONFIG + one AAU_JOINT per frame
    ├── face.bin                AAU_CONFIG + one AAU_BLENDSHAPE per frame (optional)
    └── landmarks.bin           AAU_CONFIG + one AAU_LANDMARK per frame (optional)
```

`texture_material.bin`/`texture_target_*.bin` carry no header of their own —
their `data[].type` is a real image MIME type (e.g. `image/png`), and
`libarf` never decodes them, the same way it never interprets
`mesh_positions.bin` as "a mesh": it validates shape/bytes, a consumer gives
them meaning. There is no `animations/textures.bin` — `TextureSet` has no
AAU counterpart (see "Avatar Animation Units" below), so it is a static
asset reference, not an animated track.

Each `face_blendshape_<i>.glb` is a minimal, standalone, spec-valid binary
glTF: one mesh, one primitive, a `POSITION` accessor and an indices
accessor, no materials or textures (`arf_glb.c` is the whole encoder/
decoder — a self-contained ~250 lines, not a general-purpose glTF library).
Critically, the stored positions are **absolute**, matching the same base
mesh topology as `components.meshes[0]` vertex for vertex and triangle for
triangle — not a pre-computed delta. `libarf`'s runtime still works
internally in deltas (`arfApplyBlendshapes()` is unchanged); the conversion
happens only at the read/write boundary — see the `BlendshapeSet.shapes`
note below `arf.json`'s shape for why, and the float32-precision cost of
that conversion.

One container is one tracked person. For a pipeline-produced container the
numbers are `n_verts` 18439, `n_tris` 36874, `n_joints` 127, sparse skin
nonzeros 51337, face shapes 72 — useful as test assertions, but a reader must
take them from the file, never hardcode them.

## `arf.json`

Five top-level keys, all mandatory: `preamble`, `metadata`, `structure`,
`components`, `data`.

Every component carries a numeric `id`, resolved by matching value against
the target array's own declared ids -- **not** by array position (the
General Conventions clause: "All references used in the ARF document are to
the id field of the referred item. Index-based referencing is not used in
this specification."). This library's own writer assigns sequential
`id == index` for everything it writes -- since this library only ever holds
one mesh/skin/skeleton/blendshape set/landmark set, those always get id `0`
-- but a reader must not assume that of a container it did not write itself.
`Skeleton.joints` values are also node ids, resolved the same way; AAU
`jointIndex` values, by contrast, are positional into that resolved list
(the field is named "index," not "id," and is a different kind of reference
-- see "Avatar Animation Units" below). Every component still carries its
own human-readable `name` string alongside its numeric `id`.

```
preamble:  { signature: "ARF", version: "1.0",
             supportedAnimations: ["arf-body-v1", ("arf-face-v1"), ("arf-landmark-v1")] }

metadata:  { name: <string>, id: <string> }

structure.assets: [
  { name: "body", isMain: true,
    lods: [ { name: "lod0", skins: [0], meshes: [0], skeletons: [0],
              (blendshapeSets: [0]), (landmarkSets: [0]) } ] }
]
// No field names the animation streams' location -- animations/joints.bin,
// animations/face.bin and animations/landmarks.bin are found by fixed path,
// see "Avatar Animation Units" below.

components.nodes: [ { id: <number>, name: <joint name>, mapping: <semantic path>,
                      parent: <node id>,         // absent on the root only
                      translation: [x,y,z],       // rest, parent-relative, centimetres -- optional
                      rotation: [x,y,z,w],        // rest, XYZW -- optional
                      scale: [x,y,z] } ]          // rest, non-uniform -- optional, default [1,1,1]
                    // translation/rotation/scale may instead be replaced by a
                    // single "transform": [16 numbers], mutually exclusive with them

components.skeletons: [ { id: 0, name: "skeleton0", root: <root node id>,
                          joints: [<node ids, in node order>],
                          inverseBindMatrix: <data id> } ]

components.skins:     [ { id: 0, name: "skin0", mapping: <path>,
                          skeleton: <skeleton id>, mesh: <mesh id>, weights: <data id>,
                          (textureSet: <textureSet id>) } ]

components.meshes:    [ { id: 0, name: "mesh0", path: <path>,
                          data: [<positions data id>, <indices data id>] } ]

components.blendshapeSets: [ { id: 0, name: "face_expression", baseMesh: <mesh id>,
                               shapes: [<data id>, <data id>, ...] } ]  // optional, one id per shape

components.landmarkSets: [ { id: 0, name: "landmarks", baseMesh: <mesh id>,
                             vertices: <data id> } ]  // optional

components.textureSets: [ { id: 0, name: <string>, animationInfo: [],
                            material: <data id>, materialPath: "",
                            targets: [ { id: <number>, name: <string>,
                                         texture: <data id>, texturePath: "" } ] } ]  // optional

data: [ { id: <number>, name: <string>, uri, type, byteLength } ]
```

`type` is `application/mpeg.arf.dense` or `application/mpeg.arf.sparse` for
tensors (the sparse one is this project's own extension — see "Sparse
tensor" below), `model/gltf-binary` for each `BlendshapeSet.shapes` entry
(a GLB, not a tensor — see below), or a real image MIME type for `TextureSet`
material/target entries (opaque image bytes, not tensors either). Component
references such as a `weights` or `inverseBindMatrix` field are numbers
that resolve against `data[].id`, and the entry's `uri` names the ZIP entry.

**`BlendshapeSet.shapes`**: each entry is one `data[]` id naming one GLB file
(`face_blendshape_<i>.glb`), one per blendshape target — not the single
combined delta tensor this library used before this milestone. Each GLB's
`POSITION` accessor holds that shape's **absolute** deformed vertex
positions, matching the base mesh's topology vertex for vertex and triangle
for triangle (the spec: "the topology of the baseMesh and the associated
shapes shall be identical"); the blend formula in the spec, `v_out = v_0 +
sum_i(w_i * (v_i - v_0))`, computes the delta at blend time from that
absolute position, it is not stored pre-computed. `libarf`'s runtime still
works in deltas internally (`arfApplyBlendshapes()` is unchanged) — the
absolute/delta conversion happens only at the read/write boundary, in
`arf_reader.c`/`arf_writer.c`, using the minimal glTF-binary (GLB) encoder/
decoder in `arf_glb.c` (a self-contained ~250 lines: one mesh, one
primitive, a `POSITION` accessor and an indices accessor, no materials or
textures — not a general-purpose glTF library). That conversion is not
bit-exact: `(base + delta) - base` can differ from `delta` by a float32 ULP
or so at the base mesh's coordinate magnitude (centimetres), which can be a
meaningfully large fraction of a fine facial delta — a real, usually small,
precision cost of the spec's absolute-position storage, not a bug.

`TextureSet` has no `baseMesh`/mesh field of its own; `skins[0].textureSet`
is the only link tying it to anything, so it is emitted (unlike
`Skin.blendshapeSet`/`landmarkSet`, skipped as redundant with
`BlendshapeSet`/`LandmarkSet`'s own `baseMesh`). `animationInfo` is
mandatory on `TextureSet` in the spec, but no `AnimationLink` enum value
means "texture," so it is emitted as an honestly empty array rather than a
fabricated link. `materialPath`/`texturePath` exist for formats where one
data item embeds several textures (e.g. a GLB material); since every data
item here is one flat image with nothing to locate within it, both are `""`
— this project's own convention for that case, the same kind of choice as
`Mesh.data`'s slot order.

`mesh.data`'s slot order (`[0]`=positions, `[1]`=indices) and `node.mapping`/
`skin.mapping`/`mesh.path` (this project has no verified taxonomy for the
semantic scene-graph paths the spec's companion scene-description part
defines, so a node's own name is used as an honest placeholder) are also
this library's own documented convention — see `arf_format.h` and
`doc/CONFORMANCE_GAPS.md` for this and the `TextureSet` conventions above.

### What a reader must check

Each of these, unchecked, yields a plausible but wrong avatar rather than an
error, which is why `libarf` verifies all of them at load time:

* **`data[].byteLength` against the real entry size.** A mismatch means the
  JSON and the binaries came from different runs.
* **Every component reference resolves** by matching declared `id`, and
  every `uri` exists in the ZIP. A reference is never treated as a direct
  array index.
* **Exactly one node has no `parent`**, and it resolves to the node
  `skeletons[0].root` names.
* **`skeletons[0].joints`, resolved to positions, is `[0, 1, 2, ...]`** in
  `components.nodes` order. AAU `jointIndex` values index this list
  positionally, so if the two ever disagree every joint drives the wrong
  bone, silently.
* **Every parent precedes its child** in the node list. The writer emits the
  hierarchy in that order; depending on it makes composition a single forward
  pass and rules out cycles for free.
* **Indices are in range** — triangle indices against the vertex count, skin
  weight vertices against the mesh, AAU joint indices against the skeleton.

## Dense tensor

```
int32   num_of_dims
int32[] dims                 num_of_dims entries
int32   dtype                glTF component type: 5126 FLOAT, 5125 UNSIGNED_INT
byte[]  data                 row-major, prod(dims) * component_size bytes
```

## Sparse tensor

```
int32   num_of_dims
int32[] dims
int32   valueCount
int32   itype                5125 UNSIGNED_INT, the index component type
int32   dtype                5126 FLOAT, the value component type
uint32[valueCount]  flat row-major indices
float32[valueCount] values
```

Skin weights use `dims = [n_verts, n_joints]` and a flat index of
`vertex * n_joints + joint`, so decoding is `vertex = index / n_joints`,
`joint = index % n_joints`.

Nothing in the format promises the entries arrive vertex-major, so a reader
that wants per-vertex runs has to build them rather than assume them.

## Avatar Animation Units

An animation stream is a bare concatenation of units — no stream header. All
multi-byte fields in this section are **big-endian** (see the note in
"Container" above), unlike the little-endian tensors elsewhere in the
container:

```
uint7BE unit_type, uint1 reserved   packed into one byte: (unit_type<<1)|reserved
                                     0 AAU_CONFIG, 1 AAU_BLENDSHAPE, 2 AAU_JOINT, 3 AAU_LANDMARK
uint32BE unit_length                payload bytes that follow
byte[]   payload
```

Every payload starts with a big-endian `uint32` timestamp, then its
type-specific fields:

```
AAU_CONFIG      uint32BE timestamp (always 0)
                uint8    profile_length
                byte[profile_length] profile   "arf-body-v1" | "arf-face-v1" |
                                                "arf-landmark-v1", a single
                                                length byte, no NUL
                float32BE timescale             ticks per second, i.e. fps

AAU_JOINT       uint32BE timestamp_ticks       == frame index
                uint16BE joint_set_id           the skeleton's declared id
                uint1 velocity_present, uint7 reserved   packed into one byte
                uint16BE joint_count_minus1
                { uint16BE joint_index; float32BE local_matrix[16];
                  float32BE velocity[16] if velocity_present }
                  * (joint_count_minus1 + 1)

AAU_BLENDSHAPE  uint32BE timestamp_ticks
                uint16BE blendshape_set_id      the blendshape set's declared id
                uint1 confidence_present, uint7 reserved   packed into one byte
                uint16BE blendshape_count_minus1
                { uint16BE blendshape_index; float32BE weight;
                  float32BE confidence if confidence_present }
                  * (blendshape_count_minus1 + 1)

AAU_LANDMARK    uint32BE timestamp_ticks
                uint16BE landmark_set_id        the landmark set's declared id
                uint1 velocity_present, uint1 confidence_present,
                uint1 is_3d_flag, uint5 reserved   packed into one byte
                uint16BE landmark_count_minus1
                { uint16BE landmark_index;
                  float32BE coordinates[3] if is_3d_flag else [2];
                  float32BE velocity if velocity_present;
                  float32BE confidence if confidence_present }
                  * (landmark_count_minus1 + 1)
```

The first unit of every stream is an `AAU_CONFIG`. One tick is one frame —
timestamps are integers deliberately, to avoid float drift — so wall-clock
time is `timestamp / timescale` seconds.

This library never has velocity or confidence data to emit, so the writer
always clears those presence bits; a reader still has to parse them correctly
(and discard the optional fields) for a container that sets them. The writer
always sets `is_3d_flag` (landmarks are stored as 3 floats internally
regardless of source); the reader accepts either and stores a 2D frame's
landmarks with z=0, so the in-memory shape stays uniform.

**Unknown unit types must be skipped, not treated as an error.**
`unit_length` exists precisely so a reader can step over units it does not
understand. This is the format's only forward-compatibility hook, and a reader
that rejects an unknown type will break the first time the writer gains a new
one.

## Conventions

These are the traps. Each produces a wrong-looking avatar rather than a
failure.

1. **Matrices are row-major with the translation in the last column** —
   `m[3]`, `m[7]`, `m[11]` are tx, ty, tz. That is `p' = M * p` on column
   vectors with row-major storage. OpenGL's `glUniformMatrix4fv` expects
   column-major, so transpose or pass `GL_TRUE`; getting it wrong shears the
   mesh instead of moving it.
2. **An AAU local matrix is the complete local transform** — translation,
   rotation and a uniform per-joint scale, already composed. Do **not**
   additionally apply the node's rest `translation`/`rotation` when animating.
   Those exist for drawing the rest pose and for tooling; the rest offsets are
   median-measured per person and are not guaranteed to compose back to the
   inverse bind matrices exactly.
3. **Skinning is standard LBS**: compose per-frame joint globals down the
   hierarchy from the AAU locals, multiply each by its inverse bind matrix,
   and blend by the sparse weights over `mesh_positions`.
4. **Units are centimetres.** The root joint's translation is the tracker's
   camera-frame world position; every other joint's is parent-relative.
5. **Quaternions are XYZW**, not WXYZ.
6. **+Y is up, the avatar sits at positive Z, and it faces +Z.** The first two
   follow from the camera-space framing; the third does not, and is worth
   measuring rather than assuming. On the sample, the vector from the head
   joint to the eye midpoint averages `X=+0.13 Y=+0.68 Z=+0.58` — the body
   faces the same direction it is displaced in. A viewer wants its eye on the
   **+Z** side looking back toward -Z; the -Z side shows the avatar's back.
   The pipeline's own renderer negates Y and Z after skinning because it draws
   with a fixed camera, and that flip is *not* baked into the container. A
   viewer with a real `lookAt` does not need it.
7. **Frames may be padded.** When the tracker loses a person the writer emits
   continuation frames repeating the last pose, so timestamps stay contiguous.
   A reader needs no special handling — just do not be surprised by frozen
   stretches.
8. **Facial expression is the only animated blendshape set.** Identity shape is
   baked into the rest mesh by the writer, not streamed, so a container's mesh
   is already personalised.
9. **Some frames are simply wrong** — see below. A reader has to decide whether
   to show them or repair them; it cannot detect them from the bytes.

## Glitch frames (Euler-singularity bursts)

The FK that produces `AAU_JOINT` matrices builds each joint's local rotation as
`prerotation[j] * quat(euler[j])`, where the Euler angles come from a linear
PCA decode of the model parameters. Euler angles have a singularity, and a
joint whose rotation sits near it can jump branches between frames.

The pelvis sits near it for whole clips at a time: its local rotation is a
near-180° turn away from its rest prerotation. So every so often two or three
consecutive frames carry a pelvis rotation roughly 100° off the smooth
trajectory either side of them. Because the entire skeleton hangs off the
pelvis, the body folds over sideways for those frames.

An earlier export of the committed sample clip showed this on 16 of 551 frames,
all within the first 1.5 seconds, in runs of two or three. The container
committed now is a later export that is free of it — its worst joint velocity
is 36°/frame against the earlier one's 155° — so the artifact is evidently
fixable on the producing side, but any container predating that fix still
carries it.

**These frames are not detectable as corrupt data.** The matrices are
orthogonal, unit determinant, unit scale, correctly framed — they are valid
rotations that happen to be wrong. Only their velocity distinguishes them: real
human motion in this data averages under 2° of joint rotation per frame, while
a glitch burst jumps 77–101°.

The producing project handles the same artifact downstream, in its GMR
retargeting path (`tools/gmr_retarget.py`, `despike_frames` / `despike_qpos`),
flagging frames whose root angular velocity exceeds 40°/frame and replacing
them by slerp from the nearest clean frames. That filter is applied *after* the
container, so `.arfz` files still carry the raw frames.

`libarf` offers the same repair as `arfDespikeFrames()`, opt-in — a reader that
silently rewrote the animation it was asked to read would be worse than one
that shows the glitch.

## `id_map.txt` (non-normative)

A flat `<type>\t<id>\t<name>` line per component (`node`, `mesh`, `skin`,
`skeleton`, `blendshapeSet`, `landmarkSet`, `textureSet`, `textureTarget`),
so a raw `AAU_JOINT` stream's numeric joint indices can be matched to a name
without a JSON parser. Every
component already carries its own mandatory `name` in `arf.json` — this is
purely a debugging convenience, never referenced from `data[]`/`structure`/
`components`, and a conformant reader has no reason to open it.

## Not in the format as emitted

The writer produces none of these, so a reader has nothing to handle: ISOBMFF
containers, RTP payload streaming, `MPEG_node_avatar` glTF scene integration,
authentication/biometrics, protection/DRM, LoDs, and `AnimationLink`/`mapping`
framework-conversion objects.
