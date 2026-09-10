/** @file arf_format.h
 *  @brief The on-the-wire constants and byte layouts of this library's ARF
 *         container, in one place.
 *
 *  Conformance status, see doc/CONFORMANCE_GAPS.md for the full accounting:
 *  the JSON document's component graph (numeric ids/indices, `structure` as
 *  Asset/LOD, `LandmarkSet`) and the AAU bitstream (field widths, big-endian
 *  byte order, `AAU_LANDMARK`) match ISO/IEC 23090-39 as read from the
 *  FDIS-stage text.  The sparse skin-weight tensor (the spec only defines a
 *  dense one) and `BlendshapeSet.shapes` (raw deltas here, GLB targets in the
 *  spec) are still this project's own convention, inherited from the
 *  original SAM3DBody-flavoured design.  This is NOT yet a fully
 *  certified-conformant implementation.
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
 *    data/landmark_vertices.bin  dense  [n_landmarks]          uint32   (optional)
 *    data/texture_material.bin   opaque image bytes                     (optional)
 *    data/texture_target_<i>.bin opaque image bytes, one per target     (optional)
 *    animations/joints.bin       AAU_CONFIG + one AAU_JOINT per frame
 *    animations/face.bin         AAU_CONFIG + one AAU_BLENDSHAPE per frame (optional)
 *    animations/landmarks.bin    AAU_CONFIG + one AAU_LANDMARK per frame (optional)
 *
 *  Every integer and float in the binary payloads is little-endian.
 *
 *  Texture material/target data items are opaque: their data[].type carries
 *  a real image MIME type (e.g. "image/png"), and this library never decodes
 *  them, the same way it never interprets mesh_positions.bin as "a mesh" --
 *  it validates shape/bytes, a consumer gives them meaning. There is no AAU
 *  unit for texture blend weights (TextureSet has no counterpart in the
 *  Animation Stream Format clause, unlike BlendshapeSet/LandmarkSet), so a
 *  texture set is a static asset reference, not an animated track.
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
#define ARF_ENTRY_LANDMARK_VERTICES "data/landmark_vertices.bin"
#define ARF_ENTRY_TEXTURE_MATERIAL  "data/texture_material.bin"
/* sprintf(name, ARF_ENTRY_TEXTURE_TARGET_FORMAT, targetIndex) -- one file per
 * texture target, since the count is variable, unlike every other data item
 * here which is fixed at one instance. */
#define ARF_ENTRY_TEXTURE_TARGET_FORMAT "data/texture_target_%u.bin"
#define ARF_ENTRY_JOINT_STREAM    "animations/joints.bin"
#define ARF_ENTRY_FACE_STREAM     "animations/face.bin"
#define ARF_ENTRY_LANDMARK_STREAM "animations/landmarks.bin"

/* data[].name for each data item -- descriptive only, arf.json resolves
 * component -> data references by data[].id (below), not by this string. */
#define ARF_ID_MESH_POSITIONS     "mesh_positions"
#define ARF_ID_MESH_INDICES       "mesh_indices"
#define ARF_ID_SKIN_WEIGHTS       "skin_weights"
#define ARF_ID_INVERSE_BIND       "inverse_bind_matrices"
#define ARF_ID_FACE_DELTAS        "face_blendshape_deltas"
#define ARF_ID_LANDMARK_VERTICES  "landmark_vertices"
#define ARF_ID_TEXTURE_MATERIAL   "texture_material"
/* sprintf(name, ARF_ID_TEXTURE_TARGET_FORMAT, targetIndex) */
#define ARF_ID_TEXTURE_TARGET_FORMAT "texture_target_%u"

/* data[].id for each data item -- what component fields (Skeleton.
 * inverseBindMatrix, Skin.weights, Mesh.data[], BlendshapeSet.shapes[],
 * LandmarkSet.vertices) actually reference. */
#define ARF_DATA_ID_MESH_POSITIONS  0
#define ARF_DATA_ID_MESH_INDICES    1
#define ARF_DATA_ID_SKIN_WEIGHTS    2
#define ARF_DATA_ID_INVERSE_BIND    3
#define ARF_DATA_ID_FACE_DELTAS     4
#define ARF_DATA_ID_LANDMARK_VERTICES 5
/* Texture material is id 6, targets are 7, 8, ... -- the first variable-
 * count data item this format writes; every other one above is fixed at
 * exactly one instance. */
#define ARF_DATA_ID_TEXTURE_MATERIAL 6
#define ARF_DATA_ID_TEXTURE_TARGET_FIRST 7

#define ARF_MIME_DENSE            "application/mpeg.arf.dense"
#define ARF_MIME_SPARSE           "application/mpeg.arf.sparse"

#define ARF_SIGNATURE             "ARF"
#define ARF_CONTAINER_VERSION     "1.0"
#define ARF_PROFILE_BODY          "arf-body-v1"
#define ARF_PROFILE_FACE          "arf-face-v1"
/* "arf-landmark-v1" has no established precedent to inherit -- unlike BODY/
 * FACE, which came from the original SAM3DBody writer, this is a new name
 * this project introduces, following the same pattern. */
#define ARF_PROFILE_LANDMARK      "arf-landmark-v1"
#define ARF_BLENDSHAPE_SET_ID     "face_expression"
#define ARF_LANDMARK_SET_ID       "landmarks"

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
 *  Avatar Animation Unit framing
 * ---------------------------------------------------------------------------
 *  An animation stream is a bare concatenation of units, no stream header.
 *  Unlike every other binary payload in this format, the AAU stream is
 *  BIG-ENDIAN: the spec's bitstream tables use "uimsbf" (unsigned integer,
 *  most significant bit first), the same MPEG-systems convention ISOBMFF and
 *  MPEG-2 Systems use, where it means big-endian byte order. Fields below are
 *  marked BE; anything not marked BE is a single byte, where endianness does
 *  not apply.
 *
 *    uint7BE unitType, uint1 reserved   packed into one byte: (type<<1)|reserved
 *    uint32BE unitLength                payload bytes that follow
 *    byte[]  payload
 *
 *  unitLength exists so that a reader can step over unit types it does not
 *  understand.  Skipping an unknown unit is REQUIRED behaviour, not an error --
 *  it is the format's only forward-compatibility hook.
 *
 *  Every payload starts with a big-endian uint32 timestamp, then its
 *  type-specific fields:
 *
 *    AAU_CONFIG      uint32BE timestamp (always 0)
 *                    uint8    profileLength
 *                    byte[profileLength] profile   "arf-body-v1" | "arf-face-v1",
 *                                                   no length-prefix beyond the
 *                                                   one byte above, no NUL
 *                    float32BE timescale           ticks per second == fps
 *
 *    AAU_JOINT       uint32BE timestampTicks       == frame index
 *                    uint16BE jointSetId            the skeleton's declared id
 *                    uint1 velocityPresent, uint7 reserved   packed into one byte
 *                    uint16BE jointCountMinus1
 *                    { uint16BE jointIndex; float32BE localMatrix[16];
 *                      float32BE velocity[16] if velocityPresent; }
 *                      * (jointCountMinus1 + 1)
 *
 *    AAU_BLENDSHAPE  uint32BE timestampTicks
 *                    uint16BE blendshapeSetId       the blendshape set's declared id
 *                    uint1 confidencePresent, uint7 reserved   packed into one byte
 *                    uint16BE blendshapeCountMinus1
 *                    { uint16BE blendshapeIndex; float32BE weight;
 *                      float32BE confidence if confidencePresent; }
 *                      * (blendshapeCountMinus1 + 1)
 *
 *    AAU_LANDMARK    uint32BE timestampTicks
 *                    uint16BE landmarkSetId         the landmark set's declared id
 *                    uint1 velocityPresent, uint1 confidencePresent,
 *                    uint1 is3DFlag, uint5 reserved   packed into one byte
 *                    uint16BE landmarkCountMinus1
 *                    { uint16BE landmarkIndex;
 *                      float32BE coordinates[3] if is3DFlag else [2];
 *                      float32BE velocity if velocityPresent;
 *                      float32BE confidence if confidencePresent; }
 *                      * (landmarkCountMinus1 + 1)
 *
 *  The first unit of every stream is an AAU_CONFIG.  One tick is one frame --
 *  timestamps are integers on purpose, to avoid float drift -- so wall-clock
 *  time is  timestamp / timescale  seconds.
 *
 *  This library never has velocity or confidence data to emit, so the writer
 *  always clears those presence bits; the reader still has to parse them
 *  correctly (and discard the optional fields) for a container that sets them.
 *  The writer always sets is3DFlag (this library only ever stores 3
 *  coordinates per landmark); the reader accepts either and stores a 2D
 *  frame's landmarks with z=0, so the in-memory shape stays uniform.
 * -------------------------------------------------------------------------*/

#define ARF_AAU_CONFIG      0
#define ARF_AAU_BLENDSHAPE  1
#define ARF_AAU_JOINT       2
#define ARF_AAU_LANDMARK    3

#define ARF_AAU_HEADER_BYTES 5  /* packed (type<<1)|reserved byte + uint32BE unitLength */

/* ---------------------------------------------------------------------------
 *  TextureSet / TextureTarget
 * ---------------------------------------------------------------------------
 *  components.textureSets[0] { name, id, animationInfo, material,
 *  materialPath, targets: [ { name, id, texture, texturePath } ] }.
 *
 *  animationInfo is mandatory in the spec but there is nothing parametric to
 *  link it to here (no AnimationLink enum value means "texture"), so this
 *  library emits an empty array -- present, honestly empty, rather than a
 *  fabricated link.
 *
 *  material/texture reference data items whose content is a whole opaque
 *  image; materialPath/texturePath ("indicates where the texture can be
 *  found in the item") exist for formats where one data item embeds several
 *  textures (e.g. a GLB material). Since every data item here is a single
 *  flat image with nothing to locate within it, both are emitted as "" --
 *  this project's own convention for that case, undocumented by the spec
 *  text available here, the same kind of choice as Mesh.data's slot order.
 *
 *  TextureSet has no baseMesh/mesh field of its own; skins[0].textureSet is
 *  the only link tying it to anything, so this library emits it (unlike
 *  Skin.blendshapeSet/landmarkSet, which are skipped as redundant with
 *  BlendshapeSet/LandmarkSet's own baseMesh).
 * -------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif /* ARF_FORMAT_H_INCLUDED */
