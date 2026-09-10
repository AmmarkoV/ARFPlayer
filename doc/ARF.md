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

## What this is not

ARF is ISO/IEC 23090-39 (MPEG-I Part 39), at FDIS stage as of writing. The
writer that produces these containers was designed against the published
system-level overview article, **not** the FDIS bitstream-syntax text:

> J. Regateiro, A. Trioux, Q. Avril, "The MPEG Avatar Representation Format
> (ARF): An Interoperable Container and Animation Framework for Avatars,"
> *IEEE Computer Graphics and Applications*, 2026.
> <https://ieeexplore.ieee.org/document/11667221>

The article pins down the JSON document's top-level structure, the container
options, and the shape of the animation stream format at a level sufficient to
design against. Where it does not pin down an exact bit layout, the writer made
its own explicit choice. Those choices — the AAU numeric type codes, the
config unit's field list, a byte-aligned rather than 7-bit-packed `unit_type`,
raw dense tensors instead of embedded GLB blendshape targets — are **that
project's convention, not verified spec values**.

So: this library reads SAM3DBody-flavoured ARF. `tools/validate_arf.py`, copied
here from the writer's repository, is the authoritative description of what the
writer actually emits and the numeric oracle this implementation is checked
against.

No specification text or figures are reproduced here. ISO and IEEE retain
copyright in their own publications regardless of this project's license.

## Container

A `.arfz` is a plain ZIP. All integers and floats in the binary payloads are
little-endian.

```
<stem>_<id>.arfz
├── arf.json                    the base avatar model
├── data/
│   ├── mesh_positions.bin      dense  [n_verts, 3]            float32
│   ├── mesh_indices.bin        dense  [n_tris, 3]             uint32
│   ├── skin_weights.bin        sparse dims [n_verts, n_joints]
│   ├── inv_bind_pose.bin       dense  [n_joints, 16]          float32
│   └── face_blendshapes.bin    dense  [n_shapes, n_verts, 3]  float32   (optional)
└── animations/
    ├── joints.bin              AAU_CONFIG + one AAU_JOINT per frame
    └── face.bin                AAU_CONFIG + one AAU_BLENDSHAPE per frame (optional)
```

One container is one tracked person. For a pipeline-produced container the
numbers are `n_verts` 18439, `n_tris` 36874, `n_joints` 127, sparse skin
nonzeros 51337, face shapes 72 — useful as test assertions, but a reader must
take them from the file, never hardcode them.

## `arf.json`

Five top-level keys, all mandatory: `preamble`, `metadata`, `structure`,
`components`, `data`.

```
preamble:  { signature: "ARF", version: "1.0",
             supportedAnimations: ["arf-body-v1", ("arf-face-v1")] }

metadata:  { name: <string>, id: <string> }

structure.animationStreams: [
  { id: "body_joints",     uri: "animations/joints.bin", frameworks: "arf-body-v1" },
  { id: "face_expression", uri: "animations/face.bin",   frameworks: "arf-face-v1" }  // optional
]

components.nodes: [ { id: <joint name>,
                      parent: <joint name>,     // absent on the root only
                      translation: [x,y,z],     // rest, parent-relative, centimetres
                      rotation: [x,y,z,w] } ]   // rest, XYZW

components.skeletons: [ { id: "skeleton0", root: <root joint name>,
                          joints: [<names, in node order>],
                          inverseBindMatrices: "inverse_bind_matrices" } ]

components.skins:     [ { id: "skin0", skeleton: "skeleton0", weights: "skin_weights" } ]

components.meshes:    [ { id: "mesh0", positions: "mesh_positions",
                          indices: "mesh_indices", skin: "skin0" } ]

components.blendshapeSets: [ { id: "face_expression", baseMesh: "mesh0",
                               count: <n>, deltas: "face_blendshape_deltas" } ]  // optional

data: [ { id, uri, mimeType, byteLength } ]
```

`mimeType` is `application/mpeg.arf.dense` or `application/mpeg.arf.sparse`
(the writer's convention). Component references such as `"mesh_positions"`
resolve against `data[].id`, and the entry's `uri` names the ZIP entry.

### What a reader must check

Each of these, unchecked, yields a plausible but wrong avatar rather than an
error, which is why `libarf` verifies all of them at load time:

* **`data[].byteLength` against the real entry size.** A mismatch means the
  JSON and the binaries came from different runs.
* **Every component reference resolves**, and every `uri` exists in the ZIP.
* **Exactly one node has no `parent`**, and it is the one `skeletons[0].root`
  names.
* **`skeletons[0].joints` is the same list, in the same order, as
  `components.nodes`.** AAU `jointIndex` values index this list positionally,
  so if the two ever disagree every joint drives the wrong bone, silently.
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

An animation stream is a bare concatenation of units — no stream header:

```
uint8   unit_type       0 AAU_CONFIG, 1 AAU_BLENDSHAPE, 2 AAU_JOINT
uint32  unit_length     payload bytes that follow
byte[]  payload
```

Strings inside a payload are `uint32 length` followed by that many raw UTF-8
bytes, with no terminating NUL.

```
AAU_CONFIG      uint32  timestamp (always 0)
                string  profile             "arf-body-v1" | "arf-face-v1"
                float32 timescale           ticks per second, i.e. fps

AAU_JOINT       uint32  timestamp_ticks     == frame index
                uint32  n_joints
                { uint32 joint_index; float32 local_matrix[16] } * n_joints

AAU_BLENDSHAPE  uint32  timestamp_ticks
                string  target_blendshape_set_id    "face_expression"
                uint8   has_confidence              (always 0)
                uint32  n_entries
                { uint32 blendshape_index; float32 weight } * n_entries
```

The first unit of every stream is an `AAU_CONFIG`. One tick is one frame —
timestamps are integers deliberately, to avoid float drift — so wall-clock
time is `timestamp / timescale` seconds.

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

In the committed sample this happens on 16 of 551 frames, all within the first
1.5 seconds, in runs of two or three.

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

## Not in the format as emitted

The writer produces none of these, so a reader has nothing to handle: ISOBMFF
containers, RTP payload streaming, `MPEG_node_avatar` glTF scene integration,
authentication/biometrics, protection/DRM, landmark sets, texture sets and
texture animation, LoDs, and `AnimationLink`/`mapping` framework-conversion
objects.
