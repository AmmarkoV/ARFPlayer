/** @file arf.h
 *  @brief libarf -- a small dependency-light C reader/writer for MPEG ARF
 *         avatar containers (.arfz), plus the linear blend skinning needed to
 *         turn one into a posed mesh.
 *
 *  The whole public surface is here.  Everything is plain C99 with flat,
 *  contiguous arrays and no callbacks, so it binds to ctypes and C++ without
 *  a shim layer.
 *
 *  Typical read use:
 *  @code
 *      struct arfAvatar *avatar = arfLoad("person_0.arfz");
 *      if (avatar==0) { fprintf(stderr,"%s\n",arfLastError()); return 1; }
 *
 *      struct arfPose *pose = arfPoseAllocate(avatar);
 *      for (unsigned int frame=0; frame<avatar->numberOfFrames; frame++)
 *        {
 *          arfPoseEvaluate(avatar,pose,frame);
 *          draw(pose->positions,pose->normals,avatar->mesh.indices);
 *        }
 *
 *      arfPoseFree(pose);
 *      arfFree(avatar);
 *  @endcode
 *
 *  Conventions that will silently produce a wrong-looking avatar if ignored:
 *
 *   - Every 4x4 matrix here is ROW-MAJOR with the translation in the last
 *     column (m[3], m[7], m[11]), i.e. p' = M * p on column vectors.  OpenGL's
 *     glUniformMatrix4fv wants the transpose, so pass GL_TRUE or run the
 *     matrix through arfTranspose4x4() before uploading.
 *   - A frame's local matrix is the COMPLETE local transform of that joint
 *     (translation, rotation and uniform scale already baked in).  Do not also
 *     apply the node's rest translation/rotation when animating -- those exist
 *     only to draw the rest pose and for tooling.
 *   - Units are centimetres.
 *   - Rest rotations are quaternions in XYZW order, not WXYZ.
 *   - The producing pipeline flips Y and Z after skinning to get a
 *     camera-facing orientation.  That flip is NOT baked into the container,
 *     so raw skinned output will look upside-down or back-to-front until a
 *     viewer applies it as a display convention.
 *
 *  This library reads SAM3DBody-flavoured ARF.  It is not a certified
 *  conformant ISO/IEC 23090-39 implementation -- see arf_format.h.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_H_INCLUDED
#define ARF_H_INCLUDED

#include <stddef.h>

#include "arf_format.h"

#ifdef __cplusplus
extern "C"
{
#endif

static const char arfLibraryVersion[]="0.1";

/** @brief Longest joint / avatar name we store inline.  The producing pipeline's
 *  longest joint name is 20 characters; a container that exceeds this is
 *  rejected at load time rather than silently truncated. */
#define ARF_MAX_NAME 64

/** @brief Return codes.  Anything non-zero left arfLastError() set. */
enum arfResult
{
    ARF_OK = 0,
    ARF_ERROR_ARGUMENT,
    ARF_ERROR_MEMORY,
    ARF_ERROR_IO,
    ARF_ERROR_ZIP,
    ARF_ERROR_JSON,
    ARF_ERROR_FORMAT
};

/** @brief One joint of the rest skeleton, as listed in components.nodes.
 *  Node order is also the joint order used by AAU jointIndex values, and is
 *  this node's numeric id (components.nodes[i].id == i, this project's own
 *  convention since the format only requires ids to be unique). */
struct arfNode
{
    char  name[ARF_MAX_NAME]; /**< components.nodes[i].name */
    int   parent;             /**< index into arfAvatar::nodes, -1 on the root */
    float translation[3];     /**< rest, parent-relative, centimetres */
    float rotation[4];        /**< rest, XYZW quaternion */
    float scale[3];           /**< rest, non-uniform scale, {1,1,1} if unset */
};

/** @brief The personalised rest mesh. */
struct arfMesh
{
    unsigned int  numberOfVertices;
    unsigned int  numberOfTriangles;
    float        *positions;     /**< numberOfVertices * 3 */
    unsigned int *indices;       /**< numberOfTriangles * 3 */
};

/** @brief Sparse per-vertex skin weights plus the inverse bind matrices.
 *
 *  The weights are kept in the container's own COO form (three parallel arrays
 *  of numberOfWeights entries).  vertexStart is a numberOfVertices+1 CSR-style
 *  index built by arfRebuildSkinIndex(), so the influences of vertex v are the
 *  entries [ vertexStart[v] , vertexStart[v+1] ). */
struct arfSkin
{
    unsigned int  numberOfVertices;
    unsigned int  numberOfJoints;
    unsigned int  numberOfWeights;
    unsigned int *vertexIndex;
    unsigned int *jointIndex;
    float        *weight;
    unsigned int *vertexStart;          /**< numberOfVertices+1, or 0 if not built yet */
    float        *inverseBindMatrices;  /**< numberOfJoints * 16, row-major */
};

/** @brief Optional facial expression targets, applied to the rest mesh before
 *  skinning.  Present only in containers written with face export enabled. */
struct arfBlendshapes
{
    unsigned int  numberOfShapes;
    unsigned int  numberOfVertices;
    float        *deltas;    /**< numberOfShapes * numberOfVertices * 3 */
};

/** @brief A set of tracked mesh-vertex landmarks (components.landmarkSets).
 *  Present only in containers written with landmark tracking enabled. */
struct arfLandmarks
{
    unsigned int  numberOfLandmarks;
    unsigned int *vertexIndex;   /**< numberOfLandmarks, into mesh.positions */
};

/** @brief A complete avatar container held in memory.
 *
 *  Animation is stored decoded and dense: every frame carries a local matrix
 *  for every node, so scrubbing to an arbitrary frame is a pointer offset.  A
 *  551-frame 127-joint clip costs about 4.5 MB, the same order as the file it
 *  came from. */
struct arfAvatar
{
    char name[ARF_MAX_NAME];
    char id[ARF_MAX_NAME];

    unsigned int    numberOfNodes;
    struct arfNode *nodes;
    unsigned int    rootNode;      /**< index of the one node without a parent */

    struct arfMesh  mesh;
    struct arfSkin  skin;

    /* Body track -- AAU_JOINT units of animations/joints.bin */
    float         timescale;         /**< ticks per second, i.e. frames per second */
    unsigned int  numberOfFrames;
    unsigned int *frameTimestamp;    /**< numberOfFrames, in ticks */
    float        *localMatrices;     /**< numberOfFrames * numberOfNodes * 16 */

    /* Face track -- AAU_BLENDSHAPE units of animations/face.bin, optional */
    unsigned int          hasFace;
    struct arfBlendshapes blendshapes;
    float                 faceTimescale;
    unsigned int          numberOfFaceFrames;
    unsigned int         *faceTimestamp;
    float                *blendshapeWeights;  /**< numberOfFaceFrames * numberOfShapes */

    /* Landmark track -- AAU_LANDMARK units of animations/landmarks.bin,
     * optional.  Positions are always stored as 3 floats per landmark, z=0
     * for a frame that arrived as 2D (ala_is_3d_flag off). */
    unsigned int         hasLandmarks;
    struct arfLandmarks   landmarks;
    float                 landmarkTimescale;
    unsigned int          numberOfLandmarkFrames;
    unsigned int         *landmarkTimestamp;
    float                *landmarkPositions;  /**< numberOfLandmarkFrames * numberOfLandmarks * 3 */

    /* Reserved capacity, used while building a container for writing */
    unsigned int frameCapacity;
    unsigned int faceFrameCapacity;
    unsigned int landmarkFrameCapacity;
};

/** @brief Scratch buffers for one evaluated frame.  Allocate once, reuse. */
struct arfPose
{
    unsigned int  numberOfJoints;
    unsigned int  numberOfVertices;
    float        *jointGlobals;    /**< numberOfJoints * 16, model-space joint transforms */
    float        *skinMatrices;    /**< numberOfJoints * 16, jointGlobal * inverseBind */
    float        *restPositions;   /**< numberOfVertices * 3, mesh plus blendshape deltas */
    float        *positions;       /**< numberOfVertices * 3, skinned result */
    float        *normals;         /**< numberOfVertices * 3, recomputed from positions */
};


/* ===========================================================================
 *  Errors
 * =========================================================================*/

/** @brief Human-readable description of the last failure.
 *  Never NULL.  Not thread safe -- one message slot for the whole library. */
const char *arfLastError(void);


/* ===========================================================================
 *  Reading
 * =========================================================================*/

/** @brief Load a .arfz container from disk.
 *  @param filename path to the container
 *  @retval a fully populated avatar the caller must arfFree(), or 0 on failure */
struct arfAvatar *arfLoad(const char *filename);

/** @brief Load a .arfz container already held in memory.
 *  @param bytes  the raw ZIP bytes, not retained after this call
 *  @param length number of bytes
 *  @retval a fully populated avatar the caller must arfFree(), or 0 on failure */
struct arfAvatar *arfLoadFromMemory(const void *bytes, size_t length);

/** @brief Release an avatar and everything hanging off it. */
void arfFree(struct arfAvatar *avatar);

/** @brief Clip length in seconds, from the frame count and the timescale. */
float arfDuration(const struct arfAvatar *avatar);

/** @brief Frame index nearest a wall-clock time, clamped to the clip. */
unsigned int arfFrameAtTime(const struct arfAvatar *avatar, float seconds);

/** @brief Print a summary to stdout, in the same order and units as
 *  tools/validate_arf.py reports them, so the two can be diffed. */
void arfPrintInfo(const struct arfAvatar *avatar);


/* ===========================================================================
 *  Posing -- hierarchy compose, blendshapes, linear blend skinning, normals
 * =========================================================================*/

/** @brief Allocate the scratch buffers for one frame of this avatar.
 *  Also builds the skin's CSR index if it is not built yet, which is why the
 *  avatar is not const here.
 *  @retval a pose the caller must arfPoseFree(), or 0 on failure */
struct arfPose *arfPoseAllocate(struct arfAvatar *avatar);

/** @brief Release a pose. */
void arfPoseFree(struct arfPose *pose);

/** @brief Evaluate one frame end to end: blendshapes, hierarchy compose, skin
 *  matrices, linear blend skinning and normals.  This is the whole per-frame
 *  job of a player.
 *  @param frame index into avatar->numberOfFrames
 *  @retval ARF_OK on success */
int arfPoseEvaluate(const struct arfAvatar *avatar, struct arfPose *pose, unsigned int frame);

/** @brief Compose model-space joint transforms by walking the hierarchy.
 *  @param localMatrices numberOfNodes * 16 local transforms for one frame
 *  @param globals       numberOfNodes * 16 output */
void arfComposeGlobals(const struct arfAvatar *avatar, const float *localMatrices, float *globals);

/** @brief skinMatrices[j] = globals[j] * inverseBindMatrices[j]. */
void arfComputeSkinMatrices(const struct arfAvatar *avatar, const float *globals, float *skinMatrices);

/** @brief Write the rest mesh plus the weighted blendshape deltas into positions.
 *  @param weights numberOfShapes weights, or 0 to just copy the rest mesh */
void arfApplyBlendshapes(const struct arfAvatar *avatar, const float *weights, float *positions);

/** @brief Standard linear blend skinning of restPositions into positions. */
void arfSkinVertices(const struct arfAvatar *avatar, const float *skinMatrices,
                     const float *restPositions, float *positions);

/** @brief Recompute area-weighted smooth vertex normals from posed positions.
 *
 *  Mandatory every frame.  Rest-pose normals on a bent limb point in a
 *  direction the surface no longer has, so the lit/shadowed split falls
 *  wherever the stale normal happens to cross N.L=0 -- seen as sharp shading
 *  bands cutting across limbs at arbitrary angles. */
void arfRecomputeNormals(const struct arfAvatar *avatar, const float *positions, float *normals);

/** @brief Build the skin's per-vertex CSR index.  Idempotent; called for you
 *  by arfLoad() and arfPoseAllocate(). */
int arfRebuildSkinIndex(struct arfAvatar *avatar);


/* ===========================================================================
 *  Glitch-frame repair
 * =========================================================================*/

/** @brief The threshold the producing project's own despike defaults to, in
 *  degrees of joint rotation per frame.  Real human motion stays well under
 *  it; a decoder singularity does not. */
#define ARF_DESPIKE_DEFAULT_DEGREES 40.0f

/** @brief Replace tracking-singularity glitch frames by interpolating across
 *  them.
 *
 *  The pose estimator behind these containers regresses joint rotations as
 *  Euler angles, and a joint sitting near that representation's singularity --
 *  the pelvis does, for whole clips at a time -- can jump branches for two or
 *  three frames.  The matrices stay perfectly valid; only their velocity gives
 *  them away, and the body folds up for a couple of frames.  This flags frames
 *  whose largest per-joint angular velocity exceeds the threshold and rebuilds
 *  them by slerp and lerp from the nearest clean frames either side.
 *
 *  It rewrites the avatar's animation, so it is never applied automatically --
 *  call it when you would rather see a plausible pose than the recorded one.
 *  A container saved afterwards carries the repaired track.
 *
 *  @param maxDegreesPerFrame  velocity above which a frame is suspect,
 *                             ARF_DESPIKE_DEFAULT_DEGREES is a good default
 *  @param repairedFrames      receives how many frames were rebuilt, may be 0
 *  @retval ARF_OK on success, including when nothing needed repair */
int arfDespikeFrames(struct arfAvatar *avatar, float maxDegreesPerFrame, unsigned int *repairedFrames);


/* ===========================================================================
 *  Small matrix helpers, in this library's row-major convention
 * =========================================================================*/

/** @brief result = a * b, row-major.  result may alias a or b. */
void arfMultiply4x4(float *result, const float *a, const float *b);

/** @brief result = transpose(m).  Use it to hand a matrix to OpenGL, which
 *  expects column-major storage.  result may alias m. */
void arfTranspose4x4(float *result, const float *m);

/** @brief Set m to the identity. */
void arfIdentity4x4(float *m);


/* ===========================================================================
 *  Writing
 * =========================================================================*/

/** @brief Allocate an empty avatar sized for the given geometry, ready to be
 *  filled in and handed to arfSave().  All arrays are zeroed; node ids, mesh
 *  positions, indices, weights and inverse bind matrices are the caller's to
 *  populate through the struct or through arfSetNode().
 *  @retval an avatar the caller must arfFree(), or 0 on failure */
struct arfAvatar *arfCreate(unsigned int numberOfNodes,
                            unsigned int numberOfVertices,
                            unsigned int numberOfTriangles,
                            unsigned int numberOfWeights);

/** @brief Fill in one rest-skeleton node.
 *  @param parent index of the parent node, or -1 to make this the root */
int arfSetNode(struct arfAvatar *avatar, unsigned int index, const char *name, int parent,
               const float *translation, const float *rotation);

/** @brief Append one body animation frame.
 *  @param localMatrices numberOfNodes * 16 row-major local transforms */
int arfAppendFrame(struct arfAvatar *avatar, unsigned int timestamp, const float *localMatrices);

/** @brief Attach a facial blendshape set, enabling the face track.
 *  @param deltas numberOfShapes * numberOfVertices * 3, copied */
int arfEnableFace(struct arfAvatar *avatar, unsigned int numberOfShapes, const float *deltas);

/** @brief Append one face animation frame.
 *  @param weights numberOfShapes weights */
int arfAppendFaceFrame(struct arfAvatar *avatar, unsigned int timestamp, const float *weights);

/** @brief Attach a landmark set, enabling the landmark track.
 *  @param vertexIndex numberOfLandmarks mesh-vertex indices, copied */
int arfEnableLandmarks(struct arfAvatar *avatar, unsigned int numberOfLandmarks, const unsigned int *vertexIndex);

/** @brief Append one landmark animation frame.
 *  @param positions numberOfLandmarks * 3 floats (x,y,z; z=0 for a 2D tracker) */
int arfAppendLandmarkFrame(struct arfAvatar *avatar, unsigned int timestamp, const float *positions);

/** @brief Serialise an avatar to a .arfz container.
 *  @retval ARF_OK on success */
int arfSave(const struct arfAvatar *avatar, const char *filename);

/** @brief Write a mesh out as a Wavefront OBJ, for eyeballing geometry outside
 *  a GL context.
 *  @param positions numberOfVertices * 3, or 0 to write the rest mesh */
int arfExportOBJ(const struct arfAvatar *avatar, const float *positions, const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* ARF_H_INCLUDED */
