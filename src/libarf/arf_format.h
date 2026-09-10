/** @file arf_format.h
 *  @brief The on-the-wire constants and byte layouts of this library's ARF
 *         container, in one place.
 *
 *  Conformance status, see doc/CONFORMANCE_GAPS.md for the full accounting:
 *  the JSON document's component graph (numeric ids/indices, `structure` as
 *  Asset/LOD) matches ISO/IEC 23090-39 as read from the FDIS-stage text.  The
 *  AAU bitstream (numeric type codes, field widths), the sparse skin-weight
 *  tensor (the spec only defines a dense one), and `BlendshapeSet.shapes`
 *  (raw deltas here, GLB targets in the spec) are still this project's own
 *  convention, inherited from the original SAM3DBody-flavoured design.  This
 *  is NOT yet a fully certified-conformant implementation.
 *
 *  Everything this library assumes about the byte layout is stated here, so
 *  that when a container stops loading there is exactly one file to diff
 *  against the writer instead of a hunt through the parser.  Nothing here
 *  allocates, parses or depends on anything.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_FORMAT_H_INCLUDED
#define ARF_FORMAT_H_INCLUDED

#ifdef __cplusplus
extern "C"
{
#endif

/* ---------------------------------------------------------------------------
 *  Container
 * ---------------------------------------------------------------------------
 *  A .arfz is a plain ZIP holding:
 *
 *    arf.json                    the base avatar model, see below
 *    id_map.txt                  non-normative id->name debug index, see below
 *    data/mesh_positions.bin     dense  [n_verts, 3]           float32
 *    data/mesh_indices.bin       dense  [n_tris, 3]            uint32
 *    data/skin_weights.bin       sparse dims [n_verts, n_joints]
 *    data/inv_bind_pose.bin      dense  [n_joints, 16]         float32
 *    data/face_blendshapes.bin   dense  [n_shapes, n_verts, 3] float32  (optional)
 *    animations/joints.bin       AAU_CONFIG + one AAU_JOINT per frame
 *    animations/face.bin         AAU_CONFIG + one AAU_BLENDSHAPE per frame (optional)
 *
 *  Every integer and float in the binary payloads is little-endian.
 *
 *  arf.json's `structure`/`components` use numeric ids: every component's id
 *  is its index within its own components.<array>, e.g. components.nodes[i]
 *  has id i.  Since this library only ever holds one mesh/skin/skeleton/
 *  blendshapeSet, those always get id 0.  animations/ streams are located by
 *  the fixed paths above -- the spec's Zip-container clause locates them by
 *  convention, there is no arf.json field naming them.
 * -------------------------------------------------------------------------*/

#define ARF_ENTRY_JSON            "arf.json"
#define ARF_ENTRY_ID_MAP          "id_map.txt"
#define ARF_ENTRY_MESH_POSITIONS  "data/mesh_positions.bin"
#define ARF_ENTRY_MESH_INDICES    "data/mesh_indices.bin"
#define ARF_ENTRY_SKIN_WEIGHTS    "data/skin_weights.bin"
#define ARF_ENTRY_INV_BIND_POSE   "data/inv_bind_pose.bin"
#define ARF_ENTRY_FACE_DELTAS     "data/face_blendshapes.bin"
#define ARF_ENTRY_JOINT_STREAM    "animations/joints.bin"
#define ARF_ENTRY_FACE_STREAM     "animations/face.bin"

/* data[].name for each data item -- descriptive only, arf.json resolves
 * component -> data references by data[].id (below), not by this string. */
#define ARF_ID_MESH_POSITIONS     "mesh_positions"
#define ARF_ID_MESH_INDICES       "mesh_indices"
#define ARF_ID_SKIN_WEIGHTS       "skin_weights"
#define ARF_ID_INVERSE_BIND       "inverse_bind_matrices"
#define ARF_ID_FACE_DELTAS        "face_blendshape_deltas"

/* data[].id for each data item -- what component fields (Skeleton.
 * inverseBindMatrix, Skin.weights, Mesh.data[], BlendshapeSet.shapes[])
 * actually reference. */
#define ARF_DATA_ID_MESH_POSITIONS 0
#define ARF_DATA_ID_MESH_INDICES   1
#define ARF_DATA_ID_SKIN_WEIGHTS   2
#define ARF_DATA_ID_INVERSE_BIND   3
#define ARF_DATA_ID_FACE_DELTAS    4

#define ARF_MIME_DENSE            "application/mpeg.arf.dense"
#define ARF_MIME_SPARSE           "application/mpeg.arf.sparse"

#define ARF_SIGNATURE             "ARF"
#define ARF_CONTAINER_VERSION     "1.0"
#define ARF_PROFILE_BODY          "arf-body-v1"
#define ARF_PROFILE_FACE          "arf-face-v1"
#define ARF_BLENDSHAPE_SET_ID     "face_expression"

/* ---------------------------------------------------------------------------
 *  id_map.txt (non-normative)
 * ---------------------------------------------------------------------------
 *  A flat, greppable "<type>\t<id>\t<name>" line per component, so that a raw
 *  AAU_JOINT stream's numeric joint indices can be matched to a name without
 *  a JSON parser.  Every component already carries its own mandatory `name`
 *  field in arf.json -- this is a debugging convenience only, never
 *  referenced from data[]/structure/components, and a conformant reader has
 *  no reason to open it.
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 *  Component types -- glTF accessor component-type codes, reused verbatim.
 * -------------------------------------------------------------------------*/
#define ARF_COMPONENT_FLOAT        5126
#define ARF_COMPONENT_UNSIGNED_INT 5125

/* ---------------------------------------------------------------------------
 *  Dense tensor  (writer: build_dense_tensor)
 * ---------------------------------------------------------------------------
 *    int32   numberOfDimensions
 *    int32[] dims                    numberOfDimensions entries
 *    int32   dtype                   ARF_COMPONENT_*
 *    byte[]  data                    row-major, prod(dims) * componentSize bytes
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 *  Sparse tensor (writer: build_sparse_tensor)
 * ---------------------------------------------------------------------------
 *    int32   numberOfDimensions
 *    int32[] dims
 *    int32   valueCount
 *    int32   itype                   ARF_COMPONENT_UNSIGNED_INT, index type
 *    int32   dtype                   ARF_COMPONENT_FLOAT, value type
 *    uint32[valueCount]  flat row-major indices
 *    float32[valueCount] values
 *
 *  For the skin weights the flat index is  vertex * numberOfJoints + joint,
 *  so it decodes as  vertex = index / numberOfJoints, joint = index % numberOfJoints.
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 *  Avatar Animation Unit framing (writer: append_aau)
 * ---------------------------------------------------------------------------
 *  An animation stream is a bare concatenation of units, no stream header:
 *
 *    uint8   unitType                ARF_AAU_*
 *    uint32  unitLength              payload bytes that follow
 *    byte[]  payload
 *
 *  unitLength exists so that a reader can step over unit types it does not
 *  understand.  Skipping an unknown unit is REQUIRED behaviour, not an error --
 *  it is the format's only forward-compatibility hook.
 *
 *  Strings inside a payload are  uint32 length  followed by that many raw UTF-8
 *  bytes, with no terminating NUL.
 *
 *    AAU_CONFIG      uint32  timestamp (always 0)
 *                    string  profile            "arf-body-v1" | "arf-face-v1"
 *                    float32 timescale          ticks per second == fps
 *
 *    AAU_JOINT       uint32  timestampTicks     == frame index
 *                    uint32  numberOfJoints
 *                    { uint32 jointIndex; float32 localMatrix[16]; } * numberOfJoints
 *
 *    AAU_BLENDSHAPE  uint32  timestampTicks
 *                    string  targetBlendshapeSetId    "face_expression"
 *                    uint8   hasConfidence            (always 0)
 *                    uint32  numberOfEntries
 *                    { uint32 blendshapeIndex; float32 weight; } * numberOfEntries
 *
 *  The first unit of every stream is an AAU_CONFIG.  One tick is one frame --
 *  timestamps are integers on purpose, to avoid float drift -- so wall-clock
 *  time is  timestamp / timescale  seconds.
 * -------------------------------------------------------------------------*/

#define ARF_AAU_CONFIG      0
#define ARF_AAU_BLENDSHAPE  1
#define ARF_AAU_JOINT       2

#define ARF_AAU_HEADER_BYTES 5  /* uint8 unitType + uint32 unitLength */

#ifdef __cplusplus
}
#endif

#endif /* ARF_FORMAT_H_INCLUDED */
