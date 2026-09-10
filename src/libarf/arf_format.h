/** @file arf_format.h
 *  @brief The on-the-wire constants and byte layouts of a SAM3DBody-flavoured
 *         MPEG ARF container, in one place.
 *
 *  This file exists for one reason: the writer that produces these containers
 *  lives in a different repository (SAM3DBody-cpp, src/SAM3DBODY-cpp/arf_writer.cpp)
 *  and will keep evolving.  Everything this library assumes about the byte
 *  layout is stated here, so that when a container stops loading there is
 *  exactly one file to diff against the writer instead of a hunt through the
 *  parser.  Nothing here allocates, parses or depends on anything.
 *
 *  Honesty note, inherited from the writer's own documentation: this is NOT a
 *  certified-conformant ISO/IEC 23090-39 implementation.  The layouts below
 *  were designed against the published overview article, not the FDIS
 *  bitstream-syntax text.  The AAU numeric type codes, the byte-aligned (not
 *  7-bit-packed) unit_type, and the raw dense tensors used in place of embedded
 *  GLB blendshape targets are the writer's own convention.
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
 *    data/mesh_positions.bin     dense  [n_verts, 3]           float32
 *    data/mesh_indices.bin       dense  [n_tris, 3]            uint32
 *    data/skin_weights.bin       sparse dims [n_verts, n_joints]
 *    data/inv_bind_pose.bin      dense  [n_joints, 16]         float32
 *    data/face_blendshapes.bin   dense  [n_shapes, n_verts, 3] float32  (optional)
 *    animations/joints.bin       AAU_CONFIG + one AAU_JOINT per frame
 *    animations/face.bin         AAU_CONFIG + one AAU_BLENDSHAPE per frame (optional)
 *
 *  Every integer and float in the binary payloads is little-endian.
 * -------------------------------------------------------------------------*/

#define ARF_ENTRY_JSON            "arf.json"
#define ARF_ENTRY_MESH_POSITIONS  "data/mesh_positions.bin"
#define ARF_ENTRY_MESH_INDICES    "data/mesh_indices.bin"
#define ARF_ENTRY_SKIN_WEIGHTS    "data/skin_weights.bin"
#define ARF_ENTRY_INV_BIND_POSE   "data/inv_bind_pose.bin"
#define ARF_ENTRY_FACE_DELTAS     "data/face_blendshapes.bin"
#define ARF_ENTRY_JOINT_STREAM    "animations/joints.bin"
#define ARF_ENTRY_FACE_STREAM     "animations/face.bin"

/* The ids the writer gives its data items; component references in arf.json
 * resolve against these, and the reader falls back on them when a container
 * omits an optional cross-reference. */
#define ARF_ID_MESH_POSITIONS     "mesh_positions"
#define ARF_ID_MESH_INDICES       "mesh_indices"
#define ARF_ID_SKIN_WEIGHTS       "skin_weights"
#define ARF_ID_INVERSE_BIND       "inverse_bind_matrices"
#define ARF_ID_FACE_DELTAS        "face_blendshape_deltas"

#define ARF_MIME_DENSE            "application/mpeg.arf.dense"
#define ARF_MIME_SPARSE           "application/mpeg.arf.sparse"

#define ARF_SIGNATURE             "ARF"
#define ARF_CONTAINER_VERSION     "1.0"
#define ARF_PROFILE_BODY          "arf-body-v1"
#define ARF_PROFILE_FACE          "arf-face-v1"
#define ARF_BLENDSHAPE_SET_ID     "face_expression"

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
