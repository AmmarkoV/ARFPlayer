/** @file arf_reader.c
 *  @brief Reads a .arfz container into a struct arfAvatar.
 *
 *  The order of work is: unzip, parse arf.json, resolve every component
 *  cross-reference, load the binary tensors the components point at, then
 *  decode the animation streams.  Cross-references are checked rather than
 *  assumed -- a container whose skeleton joint list disagrees with its node
 *  list, or whose AAU units address a joint that does not exist, is rejected
 *  with a message saying which, because both of those produce a plausible but
 *  silently wrong avatar if waved through.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf.h"
#include "arf_bytes.h"
#include "arf_json.h"
#include "arf_internal.h"

#include <zip.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static char arfErrorMessage[512] = "no error";

void arfSetError(const char *format, ...)
{
    va_list arguments;
    va_start(arguments,format);
    vsnprintf(arfErrorMessage,sizeof(arfErrorMessage),format,arguments);
    va_end(arguments);
}

const char *arfLastError(void)
{
    return arfErrorMessage;
}


/* ===========================================================================
 *  ZIP entries
 * =========================================================================*/

/** @brief Uncompressed size of one entry, or -1 if it is absent. */
static zip_int64_t arfEntrySize(zip_t *archive, const char *name)
{
    zip_stat_t status;
    if (zip_stat(archive,name,0,&status) != 0)            { return -1; }
    if ((status.valid & ZIP_STAT_SIZE) == 0)              { return -1; }

    return (zip_int64_t) status.size;
}

/** @brief Extract one entry to a heap block the caller frees.
 *  @retval the bytes, or 0 if the entry is absent or corrupt */
static void *arfExtractEntry(zip_t *archive, const char *name, size_t *size)
{
    zip_int64_t declared = arfEntrySize(archive,name);
    if (declared < 0)
    {
        arfSetError("container entry \"%s\" is missing or has no readable size",name);
        return 0;
    }

    zip_file_t *file = zip_fopen(archive,name,0);
    if (file==0)
    {
        arfSetError("cannot open container entry \"%s\": %s",name,zip_strerror(archive));
        return 0;
    }

    /* One extra byte so that a zero-length entry still yields a non-NULL
     * pointer, which the callers use to distinguish success from failure. */
    unsigned char *bytes = (unsigned char *) malloc((size_t) declared + 1);
    if (bytes==0)
    {
        arfSetError("out of memory inflating \"%s\" (%lld bytes)",name,(long long) declared);
        zip_fclose(file);
        return 0;
    }

    zip_int64_t got = zip_fread(file,bytes,(zip_uint64_t) declared);
    zip_fclose(file);

    if (got != declared)
    {
        arfSetError("short read on container entry \"%s\": %lld of %lld bytes",
                    name,(long long) got,(long long) declared);
        free(bytes);
        return 0;
    }

    *size = (size_t) declared;
    return bytes;
}


/* ===========================================================================
 *  Tensors
 * =========================================================================*/

/** @brief The header of a dense tensor, with the cursor left on its payload. */
struct arfDenseTensor
{
    int numberOfDimensions;
    int dims[4];
    int dtype;
};

static int arfReadDenseHeader(struct arfCursor *cursor, struct arfDenseTensor *tensor, const char *what)
{
    tensor->numberOfDimensions = arfCursorReadI32(cursor);

    if ( (tensor->numberOfDimensions < 1) || (tensor->numberOfDimensions > 4) )
    {
        arfSetError("%s: dense tensor claims %d dimensions, expected 1 to 4",what,tensor->numberOfDimensions);
        return 0;
    }

    for (int i=0; i<tensor->numberOfDimensions; i++)
    {
        tensor->dims[i] = arfCursorReadI32(cursor);
        if (tensor->dims[i] < 0)
        {
            arfSetError("%s: dense tensor dimension %d is negative",what,i);
            return 0;
        }
    }

    tensor->dtype = arfCursorReadI32(cursor);

    if (cursor->failed) { arfSetError("%s: dense tensor header is truncated",what); return 0; }
    return 1;
}

/** @brief Total element count of a dense tensor header. */
static size_t arfDenseElementCount(const struct arfDenseTensor *tensor)
{
    size_t count = 1;
    for (int i=0; i<tensor->numberOfDimensions; i++) { count *= (size_t) tensor->dims[i]; }
    return count;
}


/* ===========================================================================
 *  AAU stream framing
 * =========================================================================*/

struct arfAAUUnit
{
    unsigned int         type;
    const unsigned char *payload;
    unsigned int         length;
};

/** @brief Step to the next unit of a stream.
 *  @retval 1 if a unit was read, 0 at the end of the stream or on a short read */
static int arfAAUNext(struct arfCursor *cursor, struct arfAAUUnit *unit)
{
    if (cursor->failed)                     { return 0; }
    if (cursor->offset >= cursor->length)   { return 0; }

    unit->type   = arfCursorReadU8(cursor);
    unit->length = arfCursorReadU32(cursor);
    unit->payload = arfCursorTake(cursor,unit->length);

    return (cursor->failed==0);
}


/* ===========================================================================
 *  arf.json component resolution
 * =========================================================================*/

/** @brief Resolve a component reference (a data[].id) to its ZIP entry name.
 *  @retval the uri, or 0 if the id is not listed */
static const char *arfResolveDataURI(const struct arfJsonValue *dataArray, const char *id)
{
    if (id==0) { return 0; }

    for (unsigned int i=0; i<arfJsonCount(dataArray); i++)
    {
        const struct arfJsonValue *item = arfJsonAt(dataArray,i);
        const char *itemId = arfJsonString(arfJsonMember(item,"id"),0);

        if ( (itemId!=0) && (strcmp(itemId,id)==0) )
        {
            return arfJsonString(arfJsonMember(item,"uri"),0);
        }
    }
    return 0;
}

/** @brief Check every data item's byteLength against the real entry size.
 *  A mismatch means the JSON and the binaries came from different runs.
 *  @retval 1 if consistent */
static int arfValidateDataItems(zip_t *archive, const struct arfJsonValue *dataArray)
{
    for (unsigned int i=0; i<arfJsonCount(dataArray); i++)
    {
        const struct arfJsonValue *item = arfJsonAt(dataArray,i);
        const char *id  = arfJsonString(arfJsonMember(item,"id"),"?");
        const char *uri = arfJsonString(arfJsonMember(item,"uri"),0);

        if (uri==0) { arfSetError("data item \"%s\" has no uri",id); return 0; }

        zip_int64_t actual = arfEntrySize(archive,uri);
        if (actual < 0) { arfSetError("data item \"%s\" points at missing entry \"%s\"",id,uri); return 0; }

        const struct arfJsonValue *declared = arfJsonMember(item,"byteLength");
        if (declared!=0)
        {
            unsigned long long expected = (unsigned long long) arfJsonNumber(declared,-1.0);
            if (expected != (unsigned long long) actual)
            {
                arfSetError("data item \"%s\": byteLength %llu does not match the %llu bytes in \"%s\"",
                            id,expected,(unsigned long long) actual,uri);
                return 0;
            }
        }
    }
    return 1;
}

/** @brief Find a node by id.  @retval its index, or -1 */
static int arfFindNode(const struct arfAvatar *avatar, const char *id)
{
    if (id==0) { return -1; }

    for (unsigned int i=0; i<avatar->numberOfNodes; i++)
    {
        if (strcmp(avatar->nodes[i].id,id)==0) { return (int) i; }
    }
    return -1;
}

/** @brief Read components.nodes into the avatar and resolve the hierarchy. */
static int arfLoadNodes(struct arfAvatar *avatar, const struct arfJsonValue *components)
{
    const struct arfJsonValue *nodes = arfJsonMember(components,"nodes");
    unsigned int count = arfJsonCount(nodes);

    if (count==0) { arfSetError("components.nodes is empty or missing"); return 0; }

    avatar->nodes = (struct arfNode *) calloc(count,sizeof(struct arfNode));
    if (avatar->nodes==0) { arfSetError("out of memory allocating %u nodes",count); return 0; }
    avatar->numberOfNodes = count;

    /* Pass one records the ids, so that pass two can resolve parents by name
     * regardless of the order the parents appear in. */
    for (unsigned int i=0; i<count; i++)
    {
        const struct arfJsonValue *node = arfJsonAt(nodes,i);
        const char *id = arfJsonString(arfJsonMember(node,"id"),0);

        if (id==0) { arfSetError("components.nodes[%u] has no id",i); return 0; }
        if (strlen(id) >= ARF_MAX_NAME)
        {
            arfSetError("components.nodes[%u] id \"%s\" is longer than the %d character limit",
                        i,id,ARF_MAX_NAME-1);
            return 0;
        }

        strcpy(avatar->nodes[i].id,id);
        avatar->nodes[i].parent = -1;
        avatar->nodes[i].rotation[3] = 1.0f;
    }

    int rootCount = 0;
    for (unsigned int i=0; i<count; i++)
    {
        const struct arfJsonValue *node = arfJsonAt(nodes,i);
        struct arfNode *destination = &avatar->nodes[i];

        const char *parent = arfJsonString(arfJsonMember(node,"parent"),0);
        if (parent==0)
        {
            avatar->rootNode = i;
            rootCount++;
        }
        else
        {
            destination->parent = arfFindNode(avatar,parent);
            if (destination->parent < 0)
            {
                arfSetError("node \"%s\" names a parent \"%s\" that is not in components.nodes",
                            destination->id,parent);
                return 0;
            }
        }

        const struct arfJsonValue *translation = arfJsonMember(node,"translation");
        for (unsigned int c=0; c<3; c++)
        {
            destination->translation[c] = (float) arfJsonNumber(arfJsonAt(translation,c),0.0);
        }

        const struct arfJsonValue *rotation = arfJsonMember(node,"rotation");
        if (rotation!=0)
        {
            /* XYZW, not WXYZ -- see the conventions note in arf.h. */
            for (unsigned int c=0; c<4; c++)
            {
                destination->rotation[c] = (float) arfJsonNumber(arfJsonAt(rotation,c),(c==3)?1.0:0.0);
            }
        }
    }

    if (rootCount!=1)
    {
        arfSetError("expected exactly one node without a parent, found %d",rootCount);
        return 0;
    }

    /* Require every parent to precede its child in the node list.  The writer
     * emits the hierarchy in that order already, and depending on it lets
     * arfComposeGlobals() be a single forward pass with no scheduling and no
     * recursion.  It also rules out cycles for free: a strictly decreasing
     * parent index cannot close a loop. */
    for (unsigned int i=0; i<count; i++)
    {
        if (avatar->nodes[i].parent >= (int) i)
        {
            arfSetError("node \"%s\" at index %u has parent \"%s\" at index %d -- "
                        "parents must be listed before their children",
                        avatar->nodes[i].id,i,avatar->nodes[avatar->nodes[i].parent].id,
                        avatar->nodes[i].parent);
            return 0;
        }
    }

    return 1;
}

/** @brief Cross-check skeletons[0] against the node list.
 *
 *  AAU jointIndex values index the skeleton's joints array, and the writer
 *  emits it in node order.  If a future writer ever reorders one and not the
 *  other, every joint in the animation silently drives the wrong bone -- so
 *  the agreement is verified rather than trusted. */
static int arfValidateSkeleton(const struct arfAvatar *avatar, const struct arfJsonValue *components)
{
    const struct arfJsonValue *skeletons = arfJsonMember(components,"skeletons");
    const struct arfJsonValue *skeleton  = arfJsonAt(skeletons,0);

    if (skeleton==0) { arfSetError("components.skeletons is empty or missing"); return 0; }

    const struct arfJsonValue *joints = arfJsonMember(skeleton,"joints");
    unsigned int jointCount = arfJsonCount(joints);

    if (jointCount != avatar->numberOfNodes)
    {
        arfSetError("skeleton lists %u joints but components.nodes has %u entries",
                    jointCount,avatar->numberOfNodes);
        return 0;
    }

    for (unsigned int i=0; i<jointCount; i++)
    {
        const char *name = arfJsonString(arfJsonAt(joints,i),"");
        if (strcmp(name,avatar->nodes[i].id)!=0)
        {
            arfSetError("skeleton joint %u is \"%s\" but components.nodes[%u] is \"%s\" -- "
                        "the two orders must agree, AAU joint indices address this list",
                        i,name,i,avatar->nodes[i].id);
            return 0;
        }
    }

    const char *root = arfJsonString(arfJsonMember(skeleton,"root"),0);
    if (root!=0)
    {
        int rootIndex = arfFindNode(avatar,root);
        if (rootIndex != (int) avatar->rootNode)
        {
            arfSetError("skeleton root is \"%s\" but the parentless node is \"%s\"",
                        root,avatar->nodes[avatar->rootNode].id);
            return 0;
        }
    }

    return 1;
}

/** @brief Load the mesh positions and triangle indices. */
static int arfLoadMesh(struct arfAvatar *avatar, zip_t *archive,
                       const struct arfJsonValue *mesh, const struct arfJsonValue *dataArray)
{
    const char *positionsURI = arfResolveDataURI(dataArray,arfJsonString(arfJsonMember(mesh,"positions"),ARF_ID_MESH_POSITIONS));
    const char *indicesURI   = arfResolveDataURI(dataArray,arfJsonString(arfJsonMember(mesh,"indices"),ARF_ID_MESH_INDICES));

    if (positionsURI==0) { arfSetError("mesh positions reference does not resolve to a data item"); return 0; }
    if (indicesURI==0)   { arfSetError("mesh indices reference does not resolve to a data item");   return 0; }

    size_t size  = 0;
    void  *bytes = arfExtractEntry(archive,positionsURI,&size);
    if (bytes==0) { return 0; }

    struct arfCursor      cursor;
    struct arfDenseTensor tensor;
    arfCursorInit(&cursor,bytes,size);

    if (!arfReadDenseHeader(&cursor,&tensor,"mesh positions")) { free(bytes); return 0; }

    if ( (tensor.numberOfDimensions!=2) || (tensor.dims[1]!=3) || (tensor.dtype!=ARF_COMPONENT_FLOAT) )
    {
        arfSetError("mesh positions must be a float [n,3] tensor");
        free(bytes);
        return 0;
    }

    avatar->mesh.numberOfVertices = (unsigned int) tensor.dims[0];
    avatar->mesh.positions = (float *) malloc(arfDenseElementCount(&tensor) * sizeof(float));

    if ( (avatar->mesh.positions==0) ||
         (!arfCursorReadFloats(&cursor,avatar->mesh.positions,arfDenseElementCount(&tensor))) )
    {
        arfSetError("mesh positions payload is truncated");
        free(bytes);
        return 0;
    }
    free(bytes);

    bytes = arfExtractEntry(archive,indicesURI,&size);
    if (bytes==0) { return 0; }

    arfCursorInit(&cursor,bytes,size);
    if (!arfReadDenseHeader(&cursor,&tensor,"mesh indices")) { free(bytes); return 0; }

    if ( (tensor.numberOfDimensions!=2) || (tensor.dims[1]!=3) || (tensor.dtype!=ARF_COMPONENT_UNSIGNED_INT) )
    {
        arfSetError("mesh indices must be an unsigned int [n,3] tensor");
        free(bytes);
        return 0;
    }

    avatar->mesh.numberOfTriangles = (unsigned int) tensor.dims[0];
    avatar->mesh.indices = (unsigned int *) malloc(arfDenseElementCount(&tensor) * sizeof(unsigned int));

    if ( (avatar->mesh.indices==0) ||
         (!arfCursorReadU32s(&cursor,avatar->mesh.indices,arfDenseElementCount(&tensor))) )
    {
        arfSetError("mesh indices payload is truncated");
        free(bytes);
        return 0;
    }
    free(bytes);

    for (unsigned int i=0; i<avatar->mesh.numberOfTriangles*3; i++)
    {
        if (avatar->mesh.indices[i] >= avatar->mesh.numberOfVertices)
        {
            arfSetError("triangle index %u addresses vertex %u of %u",
                        i,avatar->mesh.indices[i],avatar->mesh.numberOfVertices);
            return 0;
        }
    }

    return 1;
}

/** @brief Load the sparse skin weights and the inverse bind matrices. */
static int arfLoadSkin(struct arfAvatar *avatar, zip_t *archive,
                       const struct arfJsonValue *components, const struct arfJsonValue *skinId,
                       const struct arfJsonValue *dataArray)
{
    const struct arfJsonValue *skins = arfJsonMember(components,"skins");
    const struct arfJsonValue *skin  = 0;
    const char *wanted = arfJsonString(skinId,0);

    for (unsigned int i=0; i<arfJsonCount(skins); i++)
    {
        const struct arfJsonValue *candidate = arfJsonAt(skins,i);
        const char *id = arfJsonString(arfJsonMember(candidate,"id"),0);

        if ( (wanted==0) || ((id!=0) && (strcmp(id,wanted)==0)) ) { skin = candidate; break; }
    }

    if (skin==0) { arfSetError("the mesh references skin \"%s\" which is not in components.skins",(wanted!=0)?wanted:"?"); return 0; }

    const char *weightsURI = arfResolveDataURI(dataArray,arfJsonString(arfJsonMember(skin,"weights"),ARF_ID_SKIN_WEIGHTS));
    if (weightsURI==0) { arfSetError("skin weights reference does not resolve to a data item"); return 0; }

    size_t size  = 0;
    void  *bytes = arfExtractEntry(archive,weightsURI,&size);
    if (bytes==0) { return 0; }

    struct arfCursor cursor;
    arfCursorInit(&cursor,bytes,size);

    int numberOfDimensions = arfCursorReadI32(&cursor);
    if (numberOfDimensions!=2)
    {
        arfSetError("skin weights must be a 2 dimensional sparse tensor, got %d",numberOfDimensions);
        free(bytes);
        return 0;
    }

    int vertexDimension = arfCursorReadI32(&cursor);
    int jointDimension  = arfCursorReadI32(&cursor);
    int valueCount      = arfCursorReadI32(&cursor);
    int itype           = arfCursorReadI32(&cursor);
    int dtype           = arfCursorReadI32(&cursor);

    if (cursor.failed) { arfSetError("skin weights header is truncated"); free(bytes); return 0; }

    if ( (itype!=ARF_COMPONENT_UNSIGNED_INT) || (dtype!=ARF_COMPONENT_FLOAT) )
    {
        arfSetError("skin weights use component types %d/%d, expected %d/%d",
                    itype,dtype,ARF_COMPONENT_UNSIGNED_INT,ARF_COMPONENT_FLOAT);
        free(bytes);
        return 0;
    }

    if ( (vertexDimension < 0) || (jointDimension < 1) || (valueCount < 0) )
    {
        arfSetError("skin weights dims [%d,%d] valueCount %d are out of range",
                    vertexDimension,jointDimension,valueCount);
        free(bytes);
        return 0;
    }

    if ((unsigned int) vertexDimension != avatar->mesh.numberOfVertices)
    {
        arfSetError("skin weights cover %d vertices but the mesh has %u",
                    vertexDimension,avatar->mesh.numberOfVertices);
        free(bytes);
        return 0;
    }

    if ((unsigned int) jointDimension != avatar->numberOfNodes)
    {
        arfSetError("skin weights span %d joints but the skeleton has %u",
                    jointDimension,avatar->numberOfNodes);
        free(bytes);
        return 0;
    }

    avatar->skin.numberOfVertices = (unsigned int) vertexDimension;
    avatar->skin.numberOfJoints   = (unsigned int) jointDimension;
    avatar->skin.numberOfWeights  = (unsigned int) valueCount;

    avatar->skin.vertexIndex = (unsigned int *) malloc((size_t) valueCount * sizeof(unsigned int) + 1);
    avatar->skin.jointIndex  = (unsigned int *) malloc((size_t) valueCount * sizeof(unsigned int) + 1);
    avatar->skin.weight      = (float *)        malloc((size_t) valueCount * sizeof(float) + 1);

    if ( (avatar->skin.vertexIndex==0) || (avatar->skin.jointIndex==0) || (avatar->skin.weight==0) )
    {
        arfSetError("out of memory allocating %d skin weights",valueCount);
        free(bytes);
        return 0;
    }

    /* Indices arrive flattened as vertex*numberOfJoints + joint, decoded here
     * into the two-array form the skinning loop wants. */
    if (!arfCursorReadU32s(&cursor,avatar->skin.vertexIndex,(size_t) valueCount))
    {
        arfSetError("skin weight indices are truncated");
        free(bytes);
        return 0;
    }

    for (int i=0; i<valueCount; i++)
    {
        unsigned int flat = avatar->skin.vertexIndex[i];
        avatar->skin.vertexIndex[i] = flat / (unsigned int) jointDimension;
        avatar->skin.jointIndex[i]  = flat % (unsigned int) jointDimension;

        if (avatar->skin.vertexIndex[i] >= avatar->mesh.numberOfVertices)
        {
            arfSetError("skin weight %d addresses vertex %u of %u",
                        i,avatar->skin.vertexIndex[i],avatar->mesh.numberOfVertices);
            free(bytes);
            return 0;
        }
    }

    if (!arfCursorReadFloats(&cursor,avatar->skin.weight,(size_t) valueCount))
    {
        arfSetError("skin weight values are truncated");
        free(bytes);
        return 0;
    }
    free(bytes);

    /* Inverse bind matrices.  The skeleton names the data item; fall back on
     * the writer's own id when it does not. */
    const struct arfJsonValue *skeleton = arfJsonAt(arfJsonMember(components,"skeletons"),0);
    const char *inverseBindURI = arfResolveDataURI(dataArray,
                                    arfJsonString(arfJsonMember(skeleton,"inverseBindMatrices"),ARF_ID_INVERSE_BIND));

    if (inverseBindURI==0) { arfSetError("inverse bind matrices reference does not resolve to a data item"); return 0; }

    bytes = arfExtractEntry(archive,inverseBindURI,&size);
    if (bytes==0) { return 0; }

    struct arfDenseTensor tensor;
    arfCursorInit(&cursor,bytes,size);

    if (!arfReadDenseHeader(&cursor,&tensor,"inverse bind matrices")) { free(bytes); return 0; }

    if ( (tensor.numberOfDimensions!=2) || (tensor.dims[1]!=16) ||
         (tensor.dtype!=ARF_COMPONENT_FLOAT) || ((unsigned int) tensor.dims[0] != avatar->numberOfNodes) )
    {
        arfSetError("inverse bind matrices must be a float [%u,16] tensor",avatar->numberOfNodes);
        free(bytes);
        return 0;
    }

    avatar->skin.inverseBindMatrices = (float *) malloc(arfDenseElementCount(&tensor) * sizeof(float));

    if ( (avatar->skin.inverseBindMatrices==0) ||
         (!arfCursorReadFloats(&cursor,avatar->skin.inverseBindMatrices,arfDenseElementCount(&tensor))) )
    {
        arfSetError("inverse bind matrix payload is truncated");
        free(bytes);
        return 0;
    }
    free(bytes);

    return 1;
}

/** @brief Load the optional facial blendshape target tensor. */
static int arfLoadBlendshapes(struct arfAvatar *avatar, zip_t *archive,
                              const struct arfJsonValue *set, const struct arfJsonValue *dataArray)
{
    const char *deltasURI = arfResolveDataURI(dataArray,arfJsonString(arfJsonMember(set,"deltas"),ARF_ID_FACE_DELTAS));
    if (deltasURI==0) { arfSetError("blendshape deltas reference does not resolve to a data item"); return 0; }

    size_t size  = 0;
    void  *bytes = arfExtractEntry(archive,deltasURI,&size);
    if (bytes==0) { return 0; }

    struct arfCursor      cursor;
    struct arfDenseTensor tensor;
    arfCursorInit(&cursor,bytes,size);

    if (!arfReadDenseHeader(&cursor,&tensor,"blendshape deltas")) { free(bytes); return 0; }

    if ( (tensor.numberOfDimensions!=3) || (tensor.dims[2]!=3) || (tensor.dtype!=ARF_COMPONENT_FLOAT) ||
         ((unsigned int) tensor.dims[1] != avatar->mesh.numberOfVertices) )
    {
        arfSetError("blendshape deltas must be a float [shapes,%u,3] tensor",avatar->mesh.numberOfVertices);
        free(bytes);
        return 0;
    }

    avatar->blendshapes.numberOfShapes   = (unsigned int) tensor.dims[0];
    avatar->blendshapes.numberOfVertices = (unsigned int) tensor.dims[1];
    avatar->blendshapes.deltas = (float *) malloc(arfDenseElementCount(&tensor) * sizeof(float));

    if ( (avatar->blendshapes.deltas==0) ||
         (!arfCursorReadFloats(&cursor,avatar->blendshapes.deltas,arfDenseElementCount(&tensor))) )
    {
        arfSetError("blendshape delta payload is truncated");
        free(bytes);
        return 0;
    }

    free(bytes);
    return 1;
}


/* ===========================================================================
 *  Animation streams
 * =========================================================================*/

/** @brief Read the AAU_CONFIG that must open every stream. */
static int arfReadStreamConfig(struct arfCursor *cursor, char *profile, float *timescale, const char *what)
{
    struct arfAAUUnit unit;
    if (!arfAAUNext(cursor,&unit))          { arfSetError("%s is empty",what); return 0; }
    if (unit.type != ARF_AAU_CONFIG)        { arfSetError("%s does not open with an AAU_CONFIG unit",what); return 0; }

    struct arfCursor payload;
    arfCursorInit(&payload,unit.payload,unit.length);

    arfCursorReadU32(&payload);  /* config timestamp, always zero */

    if (!arfCursorReadString(&payload,profile,ARF_MAX_NAME))
    {
        arfSetError("%s: AAU_CONFIG profile string is truncated or too long",what);
        return 0;
    }

    *timescale = arfCursorReadF32(&payload);

    if (payload.failed)    { arfSetError("%s: AAU_CONFIG payload is truncated",what); return 0; }
    if (!(*timescale > 0)) { arfSetError("%s: AAU_CONFIG timescale is %f, expected a positive rate",what,*timescale); return 0; }

    return 1;
}

/** @brief Decode animations/joints.bin into the avatar's dense frame arrays. */
static int arfLoadJointStream(struct arfAvatar *avatar, const void *bytes, size_t size)
{
    struct arfCursor cursor;
    char profile[ARF_MAX_NAME];

    arfCursorInit(&cursor,bytes,size);
    if (!arfReadStreamConfig(&cursor,profile,&avatar->timescale,"the joint animation stream")) { return 0; }

    /* Counted first so the frame arrays are one allocation each.  Framing is
     * five bytes per unit, so a counting pass is essentially free. */
    struct arfCursor  counting = cursor;
    struct arfAAUUnit unit;
    unsigned int      frameCount = 0;

    while (arfAAUNext(&counting,&unit))
    {
        if (unit.type == ARF_AAU_JOINT) { frameCount++; }
    }

    if (counting.failed)
    {
        arfSetError("the joint animation stream is truncated after %u frames",frameCount);
        return 0;
    }

    if (frameCount==0) { arfSetError("the joint animation stream has no AAU_JOINT frames"); return 0; }

    size_t matricesPerFrame = (size_t) avatar->numberOfNodes * 16;

    avatar->frameTimestamp = (unsigned int *) malloc((size_t) frameCount * sizeof(unsigned int));
    avatar->localMatrices  = (float *)        malloc((size_t) frameCount * matricesPerFrame * sizeof(float));

    if ( (avatar->frameTimestamp==0) || (avatar->localMatrices==0) )
    {
        arfSetError("out of memory allocating %u frames of %u joints",frameCount,avatar->numberOfNodes);
        return 0;
    }

    avatar->numberOfFrames = frameCount;
    avatar->frameCapacity  = frameCount;

    unsigned int frame = 0;
    while (arfAAUNext(&cursor,&unit))
    {
        /* Unknown unit types are stepped over, not rejected -- unitLength
         * exists precisely so that a reader can ignore what it does not know. */
        if (unit.type != ARF_AAU_JOINT) { continue; }

        struct arfCursor payload;
        arfCursorInit(&payload,unit.payload,unit.length);

        avatar->frameTimestamp[frame] = arfCursorReadU32(&payload);
        unsigned int jointCount       = arfCursorReadU32(&payload);

        float *matrices = avatar->localMatrices + (size_t) frame * matricesPerFrame;
        for (unsigned int j=0; j<avatar->numberOfNodes; j++) { arfIdentity4x4(matrices + (size_t) j*16); }

        for (unsigned int j=0; j<jointCount; j++)
        {
            unsigned int jointIndex = arfCursorReadU32(&payload);

            if (payload.failed) { break; }
            if (jointIndex >= avatar->numberOfNodes)
            {
                arfSetError("frame %u drives joint index %u but the skeleton has %u joints",
                            frame,jointIndex,avatar->numberOfNodes);
                return 0;
            }

            if (!arfCursorReadFloats(&payload,matrices + (size_t) jointIndex*16,16)) { break; }
        }

        if (payload.failed)
        {
            arfSetError("frame %u of the joint animation stream is truncated",frame);
            return 0;
        }

        frame++;
    }

    return 1;
}

/** @brief Decode animations/face.bin into the avatar's blendshape weight arrays. */
static int arfLoadFaceStream(struct arfAvatar *avatar, const void *bytes, size_t size)
{
    struct arfCursor cursor;
    char profile[ARF_MAX_NAME];

    arfCursorInit(&cursor,bytes,size);
    if (!arfReadStreamConfig(&cursor,profile,&avatar->faceTimescale,"the face animation stream")) { return 0; }

    struct arfCursor  counting = cursor;
    struct arfAAUUnit unit;
    unsigned int      frameCount = 0;

    while (arfAAUNext(&counting,&unit))
    {
        if (unit.type == ARF_AAU_BLENDSHAPE) { frameCount++; }
    }

    if (counting.failed) { arfSetError("the face animation stream is truncated"); return 0; }
    if (frameCount==0)   { arfSetError("the face animation stream has no AAU_BLENDSHAPE frames"); return 0; }

    unsigned int shapes = avatar->blendshapes.numberOfShapes;

    avatar->faceTimestamp     = (unsigned int *) malloc((size_t) frameCount * sizeof(unsigned int));
    avatar->blendshapeWeights = (float *)        calloc((size_t) frameCount * shapes,sizeof(float));

    if ( (avatar->faceTimestamp==0) || (avatar->blendshapeWeights==0) )
    {
        arfSetError("out of memory allocating %u face frames",frameCount);
        return 0;
    }

    avatar->numberOfFaceFrames = frameCount;
    avatar->faceFrameCapacity  = frameCount;

    unsigned int frame = 0;
    while (arfAAUNext(&cursor,&unit))
    {
        if (unit.type != ARF_AAU_BLENDSHAPE) { continue; }

        struct arfCursor payload;
        char             targetId[ARF_MAX_NAME];
        arfCursorInit(&payload,unit.payload,unit.length);

        avatar->faceTimestamp[frame] = arfCursorReadU32(&payload);

        if (!arfCursorReadString(&payload,targetId,sizeof(targetId)))
        {
            arfSetError("face frame %u has an unreadable blendshape set id",frame);
            return 0;
        }

        arfCursorReadU8(&payload);  /* hasConfidence, always zero so far */
        unsigned int entries = arfCursorReadU32(&payload);

        float *weights = avatar->blendshapeWeights + (size_t) frame * shapes;
        for (unsigned int e=0; e<entries; e++)
        {
            unsigned int index = arfCursorReadU32(&payload);
            float        value = arfCursorReadF32(&payload);

            if (payload.failed)  { break; }
            if (index < shapes)  { weights[index] = value; }
        }

        if (payload.failed) { arfSetError("face frame %u is truncated",frame); return 0; }

        frame++;
    }

    return 1;
}

/** @brief Find an animation stream's uri by the profile it declares. */
static const char *arfFindStreamURI(const struct arfJsonValue *structure, const char *profile)
{
    const struct arfJsonValue *streams = arfJsonMember(structure,"animationStreams");

    for (unsigned int i=0; i<arfJsonCount(streams); i++)
    {
        const struct arfJsonValue *stream = arfJsonAt(streams,i);
        const char *frameworks = arfJsonString(arfJsonMember(stream,"frameworks"),0);

        if ( (frameworks!=0) && (strcmp(frameworks,profile)==0) )
        {
            return arfJsonString(arfJsonMember(stream,"uri"),0);
        }
    }
    return 0;
}


/* ===========================================================================
 *  Entry points
 * =========================================================================*/

struct arfAvatar *arfLoadFromMemory(const void *bytes, size_t length)
{
    zip_t               *archive = 0;
    struct arfAvatar    *avatar = 0;
    struct arfJsonValue *document = 0;
    void                *jsonBytes = 0;
    void                *streamBytes = 0;

    if ( (bytes==0) || (length==0) ) { arfSetError("no container bytes were given"); return 0; }

    zip_error_t error;
    zip_error_init(&error);

    /* The buffer is read straight out of the caller's memory -- freep 0 means
     * libzip does not take ownership of it, so it must outlive this call,
     * which it does since nothing here retains the archive. */
    zip_source_t *source = zip_source_buffer_create(bytes,length,0,&error);
    if (source==0)
    {
        arfSetError("cannot wrap the container bytes: %s",zip_error_strerror(&error));
        zip_error_fini(&error);
        return 0;
    }

    archive = zip_open_from_source(source,ZIP_RDONLY,&error);
    if (archive==0)
    {
        arfSetError("not a readable ZIP container: %s",zip_error_strerror(&error));
        zip_source_free(source);
        zip_error_fini(&error);
        return 0;
    }
    zip_error_fini(&error);

    size_t jsonSize = 0;
    jsonBytes = arfExtractEntry(archive,ARF_ENTRY_JSON,&jsonSize);
    if (jsonBytes==0) { goto failed; }

    const char *jsonError = 0;
    document = arfJsonParse((const char *) jsonBytes,jsonSize,&jsonError);
    if (document==0) { arfSetError("%s is not valid JSON: %s",ARF_ENTRY_JSON,jsonError); goto failed; }

    /* The five top level keys the format's own reference reader treats as
     * mandatory.  Checking them up front turns "why is the mesh empty" into
     * "structure is missing". */
    static const char *requiredKeys[] = { "preamble","metadata","structure","components","data" };
    for (unsigned int i=0; i<5; i++)
    {
        if (arfJsonMember(document,requiredKeys[i])==0)
        {
            arfSetError("%s is missing the required top level key \"%s\"",ARF_ENTRY_JSON,requiredKeys[i]);
            goto failed;
        }
    }

    const struct arfJsonValue *preamble   = arfJsonMember(document,"preamble");
    const struct arfJsonValue *metadata   = arfJsonMember(document,"metadata");
    const struct arfJsonValue *structure  = arfJsonMember(document,"structure");
    const struct arfJsonValue *components = arfJsonMember(document,"components");
    const struct arfJsonValue *dataArray  = arfJsonMember(document,"data");

    const char *signature = arfJsonString(arfJsonMember(preamble,"signature"),"");
    if (strcmp(signature,ARF_SIGNATURE)!=0)
    {
        arfSetError("preamble.signature is \"%s\", expected \"%s\"",signature,ARF_SIGNATURE);
        goto failed;
    }

    if (!arfValidateDataItems(archive,dataArray)) { goto failed; }

    avatar = (struct arfAvatar *) calloc(1,sizeof(struct arfAvatar));
    if (avatar==0) { arfSetError("out of memory"); goto failed; }

    snprintf(avatar->name,sizeof(avatar->name),"%s",arfJsonString(arfJsonMember(metadata,"name"),"avatar"));
    snprintf(avatar->id,sizeof(avatar->id),"%s",arfJsonString(arfJsonMember(metadata,"id"),"0"));

    if (!arfLoadNodes(avatar,components))        { goto failed; }
    if (!arfValidateSkeleton(avatar,components)) { goto failed; }

    const struct arfJsonValue *mesh = arfJsonAt(arfJsonMember(components,"meshes"),0);
    if (mesh==0) { arfSetError("components.meshes is empty or missing"); goto failed; }

    if (!arfLoadMesh(avatar,archive,mesh,dataArray)) { goto failed; }
    if (!arfLoadSkin(avatar,archive,components,arfJsonMember(mesh,"skin"),dataArray)) { goto failed; }

    /* Body animation. */
    const char *jointURI = arfFindStreamURI(structure,ARF_PROFILE_BODY);
    if (jointURI==0) { jointURI = ARF_ENTRY_JOINT_STREAM; }

    size_t streamSize = 0;
    streamBytes = arfExtractEntry(archive,jointURI,&streamSize);
    if (streamBytes==0) { goto failed; }

    if (!arfLoadJointStream(avatar,streamBytes,streamSize)) { goto failed; }
    free(streamBytes);
    streamBytes = 0;

    /* Face animation, if this container carries one. */
    const struct arfJsonValue *blendshapeSet = arfJsonAt(arfJsonMember(components,"blendshapeSets"),0);
    const char *faceURI = arfFindStreamURI(structure,ARF_PROFILE_FACE);

    if ( (blendshapeSet!=0) && (faceURI!=0) )
    {
        if (!arfLoadBlendshapes(avatar,archive,blendshapeSet,dataArray)) { goto failed; }

        streamBytes = arfExtractEntry(archive,faceURI,&streamSize);
        if (streamBytes==0) { goto failed; }

        if (!arfLoadFaceStream(avatar,streamBytes,streamSize)) { goto failed; }
        free(streamBytes);
        streamBytes = 0;

        avatar->hasFace = 1;
    }

    if (arfRebuildSkinIndex(avatar)!=ARF_OK) { goto failed; }

    arfJsonFree(document);
    free(jsonBytes);
    zip_close(archive);          /* also releases the source */
    return avatar;

failed:
    if (streamBytes!=0) { free(streamBytes); }
    if (document!=0)    { arfJsonFree(document); }
    if (jsonBytes!=0)   { free(jsonBytes); }
    if (avatar!=0)      { arfFree(avatar); }
    zip_close(archive);
    return 0;
}

struct arfAvatar *arfLoad(const char *filename)
{
    if (filename==0) { arfSetError("no filename was given"); return 0; }

    FILE *file = fopen(filename,"rb");
    if (file==0) { arfSetError("cannot open \"%s\"",filename); return 0; }

    fseek(file,0,SEEK_END);
    long size = ftell(file);
    fseek(file,0,SEEK_SET);

    if (size <= 0) { arfSetError("\"%s\" is empty",filename); fclose(file); return 0; }

    void *bytes = malloc((size_t) size);
    if (bytes==0) { arfSetError("out of memory reading \"%s\"",filename); fclose(file); return 0; }

    if (fread(bytes,1,(size_t) size,file) != (size_t) size)
    {
        arfSetError("short read on \"%s\"",filename);
        free(bytes);
        fclose(file);
        return 0;
    }
    fclose(file);

    struct arfAvatar *avatar = arfLoadFromMemory(bytes,(size_t) size);
    free(bytes);
    return avatar;
}

void arfFree(struct arfAvatar *avatar)
{
    if (avatar==0) { return; }

    free(avatar->nodes);
    free(avatar->mesh.positions);
    free(avatar->mesh.indices);
    free(avatar->skin.vertexIndex);
    free(avatar->skin.jointIndex);
    free(avatar->skin.weight);
    free(avatar->skin.vertexStart);
    free(avatar->skin.inverseBindMatrices);
    free(avatar->blendshapes.deltas);
    free(avatar->frameTimestamp);
    free(avatar->localMatrices);
    free(avatar->faceTimestamp);
    free(avatar->blendshapeWeights);
    free(avatar);
}

float arfDuration(const struct arfAvatar *avatar)
{
    if ( (avatar==0) || (avatar->timescale <= 0) ) { return 0.0f; }
    return (float) avatar->numberOfFrames / avatar->timescale;
}

unsigned int arfFrameAtTime(const struct arfAvatar *avatar, float seconds)
{
    if ( (avatar==0) || (avatar->numberOfFrames==0) ) { return 0; }
    if (seconds <= 0.0f)                              { return 0; }

    long frame = (long) (seconds * avatar->timescale);
    if (frame < 0)                                       { return 0; }
    if (frame >= (long) avatar->numberOfFrames)          { return avatar->numberOfFrames - 1; }

    return (unsigned int) frame;
}

void arfPrintInfo(const struct arfAvatar *avatar)
{
    if (avatar==0) { return; }

    printf("avatar        : \"%s\" id=\"%s\"\n",avatar->name,avatar->id);
    printf("skeleton      : %u nodes, root=\"%s\"\n",avatar->numberOfNodes,avatar->nodes[avatar->rootNode].id);
    printf("mesh          : %u vertices, %u triangles\n",avatar->mesh.numberOfVertices,avatar->mesh.numberOfTriangles);

    float minimum[3] = {  1e30f,  1e30f,  1e30f };
    float maximum[3] = { -1e30f, -1e30f, -1e30f };

    for (unsigned int v=0; v<avatar->mesh.numberOfVertices; v++)
    {
        for (unsigned int c=0; c<3; c++)
        {
            float value = avatar->mesh.positions[v*3+c];
            if (value < minimum[c]) { minimum[c] = value; }
            if (value > maximum[c]) { maximum[c] = value; }
        }
    }

    printf("mesh bbox     : X=[%.2f,%.2f] Y=[%.2f,%.2f] Z=[%.2f,%.2f] cm\n",
           minimum[0],maximum[0],minimum[1],maximum[1],minimum[2],maximum[2]);

    /* Weight sums are the cheapest tell that a skin decoded correctly -- an
     * off-by-one in the flat index decode leaves them scattered around 1.0. */
    unsigned int outOfRange = 0;
    for (unsigned int v=0; v<avatar->skin.numberOfVertices; v++)
    {
        float sum = 0.0f;
        for (unsigned int e=avatar->skin.vertexStart[v]; e<avatar->skin.vertexStart[v+1]; e++)
        {
            sum += avatar->skin.weight[e];
        }
        if ( (sum < 0.99f) || (sum > 1.01f) ) { outOfRange++; }
    }

    printf("skin          : %u nonzero weights over [%u,%u], %u vertices with a weight sum outside [0.99,1.01]\n",
           avatar->skin.numberOfWeights,avatar->skin.numberOfVertices,avatar->skin.numberOfJoints,outOfRange);

    printf("body track    : %u frames at %.3f ticks/s, %.2f s\n",
           avatar->numberOfFrames,avatar->timescale,arfDuration(avatar));

    if (avatar->numberOfFrames > 0)
    {
        unsigned int root = avatar->rootNode;
        float rootMinimum[3] = {  1e30f,  1e30f,  1e30f };
        float rootMaximum[3] = { -1e30f, -1e30f, -1e30f };

        for (unsigned int f=0; f<avatar->numberOfFrames; f++)
        {
            const float *matrix = avatar->localMatrices + ((size_t) f * avatar->numberOfNodes + root) * 16;

            /* Row-major with the translation in the last column. */
            float translation[3] = { matrix[3], matrix[7], matrix[11] };
            for (unsigned int c=0; c<3; c++)
            {
                if (translation[c] < rootMinimum[c]) { rootMinimum[c] = translation[c]; }
                if (translation[c] > rootMaximum[c]) { rootMaximum[c] = translation[c]; }
            }
        }

        printf("root path     : \"%s\" X=[%.1f,%.1f] Y=[%.1f,%.1f] Z=[%.1f,%.1f] cm\n",
               avatar->nodes[root].id,
               rootMinimum[0],rootMaximum[0],rootMinimum[1],rootMaximum[1],rootMinimum[2],rootMaximum[2]);
    }

    if (avatar->hasFace)
    {
        printf("face track    : %u blendshapes, %u frames at %.3f ticks/s\n",
               avatar->blendshapes.numberOfShapes,avatar->numberOfFaceFrames,avatar->faceTimescale);
    }
    else
    {
        printf("face track    : absent\n");
    }
}
