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
#include "arf_glb.h"
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
 *
 *  The header's first byte is (unitType<<1)|reserved, seven bits of type
 *  packed MSB-first with a one-bit reserved field -- not a raw type byte --
 *  and unitLength is big-endian, per the AAU stream's uimsbf convention. See
 *  the "Avatar Animation Unit framing" note in arf_format.h.
 *  @retval 1 if a unit was read, 0 at the end of the stream or on a short read */
static int arfAAUNext(struct arfCursor *cursor, struct arfAAUUnit *unit)
{
    if (cursor->failed)                     { return 0; }
    if (cursor->offset >= cursor->length)   { return 0; }

    unsigned int header = arfCursorReadU8(cursor);
    unit->type    = header >> 1;
    unit->length  = arfCursorReadU32BE(cursor);
    unit->payload = arfCursorTake(cursor,unit->length);

    return (cursor->failed==0);
}


/* ===========================================================================
 *  arf.json component resolution
 * =========================================================================*/

/** @brief Resolve a component reference (a data[].id, numeric) to its ZIP
 *  entry name.  @retval the uri, or 0 if the id is not listed */
static const char *arfResolveDataURI(const struct arfJsonValue *dataArray, int id)
{
    if (id < 0) { return 0; }

    for (unsigned int i=0; i<arfJsonCount(dataArray); i++)
    {
        const struct arfJsonValue *item = arfJsonAt(dataArray,i);
        const struct arfJsonValue *itemId = arfJsonMember(item,"id");

        if ( (itemId!=0) && ((int) arfJsonNumber(itemId,-1.0) == id) )
        {
            return arfJsonString(arfJsonMember(item,"uri"),0);
        }
    }
    return 0;
}

/** @brief Resolve a component reference (a data[].id, numeric) to its
 *  data[].type (MIME) string.  @retval the type, or 0 if the id is not listed */
static const char *arfResolveDataType(const struct arfJsonValue *dataArray, int id)
{
    if (id < 0) { return 0; }

    for (unsigned int i=0; i<arfJsonCount(dataArray); i++)
    {
        const struct arfJsonValue *item = arfJsonAt(dataArray,i);
        const struct arfJsonValue *itemId = arfJsonMember(item,"id");

        if ( (itemId!=0) && ((int) arfJsonNumber(itemId,-1.0) == id) )
        {
            return arfJsonString(arfJsonMember(item,"type"),0);
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
        int         id  = (int) arfJsonNumber(arfJsonMember(item,"id"),-1.0);
        const char *uri = arfJsonString(arfJsonMember(item,"uri"),0);

        if (uri==0) { arfSetError("data item %d has no uri",id); return 0; }

        zip_int64_t actual = arfEntrySize(archive,uri);
        if (actual < 0) { arfSetError("data item %d points at missing entry \"%s\"",id,uri); return 0; }

        const struct arfJsonValue *declared = arfJsonMember(item,"byteLength");
        if (declared!=0)
        {
            unsigned long long expected = (unsigned long long) arfJsonNumber(declared,-1.0);
            if (expected != (unsigned long long) actual)
            {
                arfSetError("data item %d: byteLength %llu does not match the %llu bytes in \"%s\"",
                            id,expected,(unsigned long long) actual,uri);
                return 0;
            }
        }
    }
    return 1;
}

/** @brief Decompose a row-major 4x4 affine matrix (translation in the last
 *  column) into translation, an XYZW rotation quaternion and a non-uniform
 *  scale -- the reader-side counterpart of a Node's `transform` field, which
 *  the spec allows as an alternative to TRS (mutually exclusive with it).
 *  Nothing downstream of node loading consumes rest translation/rotation/
 *  scale at all (arfComposeGlobals works from the per-frame baked AAU
 *  matrices only), so normalizing to TRS here costs nothing at runtime. */
static void arfDecomposeTransform(const float *m, float *translation, float *rotation, float *scale)
{
    translation[0] = m[3];
    translation[1] = m[7];
    translation[2] = m[11];

    float xAxis[3] = { m[0], m[4], m[8]  };
    float yAxis[3] = { m[1], m[5], m[9]  };
    float zAxis[3] = { m[2], m[6], m[10] };

    scale[0] = sqrtf(xAxis[0]*xAxis[0] + xAxis[1]*xAxis[1] + xAxis[2]*xAxis[2]);
    scale[1] = sqrtf(yAxis[0]*yAxis[0] + yAxis[1]*yAxis[1] + yAxis[2]*yAxis[2]);
    scale[2] = sqrtf(zAxis[0]*zAxis[0] + zAxis[1]*zAxis[1] + zAxis[2]*zAxis[2]);

    float sx = (scale[0] > 1e-8f) ? 1.0f/scale[0] : 0.0f;
    float sy = (scale[1] > 1e-8f) ? 1.0f/scale[1] : 0.0f;
    float sz = (scale[2] > 1e-8f) ? 1.0f/scale[2] : 0.0f;

    /* Orthonormalized 3x3 rotation, rRC = row R, column C. */
    float r00=xAxis[0]*sx, r10=xAxis[1]*sx, r20=xAxis[2]*sx;
    float r01=yAxis[0]*sy, r11=yAxis[1]*sy, r21=yAxis[2]*sy;
    float r02=zAxis[0]*sz, r12=zAxis[1]*sz, r22=zAxis[2]*sz;

    float trace = r00 + r11 + r22;
    float qx,qy,qz,qw,s;

    if (trace > 0.0f)
    {
        s = sqrtf(trace+1.0f) * 2.0f;
        qw = 0.25f*s;  qx = (r21-r12)/s;  qy = (r02-r20)/s;  qz = (r10-r01)/s;
    }
    else if ( (r00>r11) && (r00>r22) )
    {
        s = sqrtf(1.0f+r00-r11-r22) * 2.0f;
        qw = (r21-r12)/s;  qx = 0.25f*s;  qy = (r01+r10)/s;  qz = (r02+r20)/s;
    }
    else if (r11>r22)
    {
        s = sqrtf(1.0f+r11-r00-r22) * 2.0f;
        qw = (r02-r20)/s;  qx = (r01+r10)/s;  qy = 0.25f*s;  qz = (r12+r21)/s;
    }
    else
    {
        s = sqrtf(1.0f+r22-r00-r11) * 2.0f;
        qw = (r10-r01)/s;  qx = (r02+r20)/s;  qy = (r12+r21)/s;  qz = 0.25f*s;
    }

    rotation[0]=qx; rotation[1]=qy; rotation[2]=qz; rotation[3]=qw;
}

/** @brief Position of the array element whose declared numeric id matches
 *  `wanted`, or -1.
 *
 *  The spec's General Conventions clause is explicit: "All references used
 *  in the ARF document are to the id field of the referred item. Index-based
 *  referencing is not used in this specification." So a reference's numeric
 *  value must always be looked up against the target array's own declared
 *  `id` fields, never used as a direct index -- this project's writer
 *  happens to assign id == array position, but a reader must not assume
 *  that of a container it did not write itself. */
static int arfFindIdIndex(const int *ids, unsigned int count, int wanted)
{
    for (unsigned int i=0; i<count; i++) { if (ids[i]==wanted) { return (int) i; } }
    return -1;
}

/** @brief Read every declared `array[i].id`, positionally.
 *  @retval a count-sized array the caller frees, or 0 on allocation failure */
static int *arfReadDeclaredIds(const struct arfJsonValue *array, unsigned int count)
{
    int *ids = (int *) malloc((size_t) count * sizeof(int));
    if (ids==0) { return 0; }

    for (unsigned int i=0; i<count; i++)
    {
        ids[i] = (int) arfJsonNumber(arfJsonMember(arfJsonAt(array,i),"id"),-1.0);
    }
    return ids;
}

/** @brief Read components.nodes into the avatar and resolve the hierarchy.
 *  `parent` values are node ids, looked up against the declared ids of the
 *  other nodes, not used as direct indices -- see arfFindIdIndex(). Node
 *  storage order still follows the JSON array order, and every parent must
 *  still precede its child in that order (checked below); it is only the
 *  `id` values themselves that need not equal position. */
static int arfLoadNodes(struct arfAvatar *avatar, const struct arfJsonValue *components)
{
    const struct arfJsonValue *nodes = arfJsonMember(components,"nodes");
    unsigned int count = arfJsonCount(nodes);

    if (count==0) { arfSetError("components.nodes is empty or missing"); return 0; }

    avatar->nodes = (struct arfNode *) calloc(count,sizeof(struct arfNode));
    if (avatar->nodes==0) { arfSetError("out of memory allocating %u nodes",count); return 0; }
    avatar->numberOfNodes = count;

    int *declaredIds = arfReadDeclaredIds(nodes,count);
    if (declaredIds==0) { arfSetError("out of memory allocating %u node ids",count); return 0; }

    int rootCount = 0;
    for (unsigned int i=0; i<count; i++)
    {
        const struct arfJsonValue *node = arfJsonAt(nodes,i);
        struct arfNode *destination = &avatar->nodes[i];

        const char *name = arfJsonString(arfJsonMember(node,"name"),0);
        if (name==0) { arfSetError("components.nodes[%u] has no name",i); free(declaredIds); return 0; }
        if (strlen(name) >= ARF_MAX_NAME)
        {
            arfSetError("components.nodes[%u] name \"%s\" is longer than the %d character limit",
                        i,name,ARF_MAX_NAME-1);
            free(declaredIds);
            return 0;
        }
        strcpy(destination->name,name);

        const struct arfJsonValue *parent = arfJsonMember(node,"parent");
        if (parent==0)
        {
            destination->parent = -1;
            avatar->rootNode = i;
            rootCount++;
        }
        else
        {
            int parentId = (int) arfJsonNumber(parent,-1.0);
            destination->parent = arfFindIdIndex(declaredIds,count,parentId);
            if (destination->parent < 0)
            {
                arfSetError("node \"%s\" has parent id %d, which is not a declared node id",
                            name,parentId);
                free(declaredIds);
                return 0;
            }
        }

        const struct arfJsonValue *transform = arfJsonMember(node,"transform");
        if (transform!=0)
        {
            /* Alternative to TRS, mutually exclusive with it -- decompose to
             * the canonical TRS this library stores. */
            float matrix[16];
            for (unsigned int c=0; c<16; c++) { matrix[c] = (float) arfJsonNumber(arfJsonAt(transform,c),0.0); }
            arfDecomposeTransform(matrix,destination->translation,destination->rotation,destination->scale);
        }
        else
        {
            const struct arfJsonValue *translation = arfJsonMember(node,"translation");
            for (unsigned int c=0; c<3; c++)
            {
                destination->translation[c] = (float) arfJsonNumber(arfJsonAt(translation,c),0.0);
            }

            const struct arfJsonValue *rotation = arfJsonMember(node,"rotation");
            /* XYZW, not WXYZ -- see the conventions note in arf.h. */
            for (unsigned int c=0; c<4; c++)
            {
                destination->rotation[c] = (rotation!=0)
                    ? (float) arfJsonNumber(arfJsonAt(rotation,c),(c==3)?1.0:0.0) : ((c==3)?1.0f:0.0f);
            }

            const struct arfJsonValue *scale = arfJsonMember(node,"scale");
            for (unsigned int c=0; c<3; c++)
            {
                destination->scale[c] = (scale!=0) ? (float) arfJsonNumber(arfJsonAt(scale,c),1.0) : 1.0f;
            }
        }
    }

    free(declaredIds);

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
                        avatar->nodes[i].name,i,avatar->nodes[avatar->nodes[i].parent].name,
                        avatar->nodes[i].parent);
            return 0;
        }
    }

    return 1;
}

/** @brief Cross-check skeletons[0] against the node list.
 *
 *  `joints`/`root` are node ids (looked up, not indexed -- see
 *  arfFindIdIndex()), but AAU jointIndex values are positional into this
 *  list, so the *resolved position* of joints[i] must equal i regardless of
 *  what id value it names. If a future writer ever reorders one and not the
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

    const struct arfJsonValue *nodes = arfJsonMember(components,"nodes");
    int *nodeIds = arfReadDeclaredIds(nodes,avatar->numberOfNodes);
    if (nodeIds==0) { arfSetError("out of memory allocating %u node ids",avatar->numberOfNodes); return 0; }

    for (unsigned int i=0; i<jointCount; i++)
    {
        int jointId = (int) arfJsonNumber(arfJsonAt(joints,i),-1.0);
        int position = arfFindIdIndex(nodeIds,avatar->numberOfNodes,jointId);

        if (position != (int) i)
        {
            arfSetError("skeleton joint %u is node id %d, at position %d in components.nodes -- "
                        "AAU joint indices assume this list agrees with components.nodes order",
                        i,jointId,position);
            free(nodeIds);
            return 0;
        }
    }

    int rootId = (int) arfJsonNumber(arfJsonMember(skeleton,"root"),-1.0);
    int rootPosition = arfFindIdIndex(nodeIds,avatar->numberOfNodes,rootId);
    free(nodeIds);

    if (rootPosition != (int) avatar->rootNode)
    {
        arfSetError("skeleton root is node id %d but the parentless node is \"%s\" (position %u)",
                    rootId,avatar->nodes[avatar->rootNode].name,avatar->rootNode);
        return 0;
    }

    return 1;
}

/** @brief Load the mesh positions and triangle indices.
 *
 *  Mesh.data is an array of numeric data-item references; the spec text
 *  available to this project does not pin down what each slot means beyond
 *  "mesh data", so [0]=positions, [1]=indices is this library's own
 *  documented convention (arf_format.h), not a verified spec order. */
static int arfLoadMesh(struct arfAvatar *avatar, zip_t *archive,
                       const struct arfJsonValue *mesh, const struct arfJsonValue *dataArray)
{
    const struct arfJsonValue *data = arfJsonMember(mesh,"data");

    if (arfJsonCount(data) < 2) { arfSetError("mesh.data must list at least 2 data items (positions, indices)"); return 0; }

    const char *positionsURI = arfResolveDataURI(dataArray,(int) arfJsonNumber(arfJsonAt(data,0),-1.0));
    const char *indicesURI   = arfResolveDataURI(dataArray,(int) arfJsonNumber(arfJsonAt(data,1),-1.0));

    if (positionsURI==0) { arfSetError("mesh.data[0] (positions) does not resolve to a data item"); return 0; }
    if (indicesURI==0)   { arfSetError("mesh.data[1] (indices) does not resolve to a data item");   return 0; }

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

/** @brief Load the sparse skin weights and the inverse bind matrices.
 *  @param skin components.skins[0], already found and cross-checked by the
 *  caller against the mesh and skeleton it names. */
static int arfLoadSkin(struct arfAvatar *avatar, zip_t *archive,
                       const struct arfJsonValue *components, const struct arfJsonValue *skin,
                       const struct arfJsonValue *dataArray)
{
    int weightsId = (int) arfJsonNumber(arfJsonMember(skin,"weights"),-1.0);
    const char *weightsURI = arfResolveDataURI(dataArray,weightsId);
    if (weightsURI==0) { arfSetError("skin.weights (data id %d) does not resolve to a data item",weightsId); return 0; }

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

    /* Inverse bind matrices, named by the skeleton (singular field in the
     * spec: inverseBindMatrix, one data item covering every joint). */
    const struct arfJsonValue *skeleton = arfJsonAt(arfJsonMember(components,"skeletons"),0);
    int inverseBindId = (int) arfJsonNumber(arfJsonMember(skeleton,"inverseBindMatrix"),-1.0);
    const char *inverseBindURI = arfResolveDataURI(dataArray,inverseBindId);

    if (inverseBindURI==0) { arfSetError("skeleton.inverseBindMatrix (data id %d) does not resolve to a data item",inverseBindId); return 0; }

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

/** @brief Load the optional facial blendshape target tensor.
 *
 *  Conformant BlendshapeSet.shapes is an array of per-shape GLB geometry
 *  references; this library still writes/reads its own single combined
 *  dense delta tensor as shapes[0] -- see doc/CONFORMANCE_GAPS.md, GLB
 *  targets are a later milestone, not this one. */
/** @brief Load every blendshape target GLB and convert its absolute
 *  positions back to the delta form this library's runtime uses -- see the
 *  BlendshapeSet.shapes note in arf_format.h for why the on-the-wire and
 *  in-memory representations differ. */
static int arfLoadBlendshapes(struct arfAvatar *avatar, zip_t *archive,
                              const struct arfJsonValue *set, const struct arfJsonValue *dataArray)
{
    const struct arfJsonValue *shapes = arfJsonMember(set,"shapes");
    unsigned int shapeCount = arfJsonCount(shapes);
    unsigned int vertexCount = avatar->mesh.numberOfVertices;

    if (shapeCount==0) { arfSetError("blendshapeSet.shapes is empty or missing"); return 0; }

    avatar->blendshapes.deltas = (float *) malloc((size_t) shapeCount * vertexCount * 3 * sizeof(float));
    if (avatar->blendshapes.deltas==0) { arfSetError("out of memory allocating %u blendshape targets",shapeCount); return 0; }

    avatar->blendshapes.numberOfShapes   = shapeCount;
    avatar->blendshapes.numberOfVertices = vertexCount;

    for (unsigned int s=0; s<shapeCount; s++)
    {
        int shapeId = (int) arfJsonNumber(arfJsonAt(shapes,s),-1.0);
        const char *shapeURI = arfResolveDataURI(dataArray,shapeId);
        if (shapeURI==0) { arfSetError("blendshapeSet.shapes[%u] (data id %d) does not resolve to a data item",s,shapeId); return 0; }

        size_t size  = 0;
        void  *bytes = arfExtractEntry(archive,shapeURI,&size);
        if (bytes==0) { return 0; }

        char what[64];
        snprintf(what,sizeof(what),"blendshape %u",s);

        float        *shapePositions      = 0;
        unsigned int  shapeVertexCount    = 0;
        unsigned int *shapeIndices        = 0;
        unsigned int  shapeTriangleCount  = 0;

        int ok = arfGlbReadMesh(bytes,size,what,&shapePositions,&shapeVertexCount,&shapeIndices,&shapeTriangleCount);
        free(bytes);
        if (!ok) { return 0; }

        if ( (shapeVertexCount != vertexCount) || (shapeTriangleCount != avatar->mesh.numberOfTriangles) )
        {
            arfSetError("blendshape %u has %u vertices/%u triangles, the base mesh has %u/%u -- "
                        "the spec requires identical topology",
                        s,shapeVertexCount,shapeTriangleCount,vertexCount,avatar->mesh.numberOfTriangles);
            free(shapePositions);
            free(shapeIndices);
            return 0;
        }

        if (memcmp(shapeIndices,avatar->mesh.indices,(size_t) avatar->mesh.numberOfTriangles * 3 * sizeof(unsigned int)) != 0)
        {
            arfSetError("blendshape %u's triangle indices do not match the base mesh -- "
                        "the spec requires identical topology",s);
            free(shapePositions);
            free(shapeIndices);
            return 0;
        }

        float *delta = avatar->blendshapes.deltas + (size_t) s * vertexCount * 3;
        for (size_t i=0; i<(size_t) vertexCount * 3; i++) { delta[i] = shapePositions[i] - avatar->mesh.positions[i]; }

        free(shapePositions);
        free(shapeIndices);
    }

    return 1;
}

/** @brief Load the optional set of tracked mesh-vertex landmarks.
 *  LandmarkSet.vertices is a dense [n] uint32 tensor of mesh vertex indices. */
static int arfLoadLandmarks(struct arfAvatar *avatar, zip_t *archive,
                            const struct arfJsonValue *set, const struct arfJsonValue *dataArray)
{
    int verticesId = (int) arfJsonNumber(arfJsonMember(set,"vertices"),-1.0);
    const char *verticesURI = arfResolveDataURI(dataArray,verticesId);
    if (verticesURI==0) { arfSetError("landmarkSet.vertices (data id %d) does not resolve to a data item",verticesId); return 0; }

    size_t size  = 0;
    void  *bytes = arfExtractEntry(archive,verticesURI,&size);
    if (bytes==0) { return 0; }

    struct arfCursor      cursor;
    struct arfDenseTensor tensor;
    arfCursorInit(&cursor,bytes,size);

    if (!arfReadDenseHeader(&cursor,&tensor,"landmark vertices")) { free(bytes); return 0; }

    if ( (tensor.numberOfDimensions!=1) || (tensor.dtype!=ARF_COMPONENT_UNSIGNED_INT) )
    {
        arfSetError("landmark vertices must be an unsigned int [n] tensor");
        free(bytes);
        return 0;
    }

    avatar->landmarks.numberOfLandmarks = (unsigned int) tensor.dims[0];
    avatar->landmarks.vertexIndex = (unsigned int *) malloc(arfDenseElementCount(&tensor) * sizeof(unsigned int));

    if ( (avatar->landmarks.vertexIndex==0) ||
         (!arfCursorReadU32s(&cursor,avatar->landmarks.vertexIndex,arfDenseElementCount(&tensor))) )
    {
        arfSetError("landmark vertices payload is truncated");
        free(bytes);
        return 0;
    }
    free(bytes);

    for (unsigned int i=0; i<avatar->landmarks.numberOfLandmarks; i++)
    {
        if (avatar->landmarks.vertexIndex[i] >= avatar->mesh.numberOfVertices)
        {
            arfSetError("landmark %u addresses vertex %u of %u",
                        i,avatar->landmarks.vertexIndex[i],avatar->mesh.numberOfVertices);
            return 0;
        }
    }

    return 1;
}

/** @brief Load the optional texture set: a base material image plus its
 *  texture targets, carried as opaque bytes -- this library never decodes
 *  them.  There is no AAU counterpart to load alongside this, unlike
 *  BlendshapeSet/LandmarkSet: TextureSet has no animation stream in the
 *  spec, so this is the whole of it. */
static int arfLoadTextureSet(struct arfAvatar *avatar, zip_t *archive,
                             const struct arfJsonValue *set, const struct arfJsonValue *dataArray)
{
    const char *name = arfJsonString(arfJsonMember(set,"name"),"textureSet0");
    snprintf(avatar->textureSet.name,sizeof(avatar->textureSet.name),"%s",name);

    int materialId = (int) arfJsonNumber(arfJsonMember(set,"material"),-1.0);
    const char *materialURI  = arfResolveDataURI(dataArray,materialId);
    const char *materialType = arfResolveDataType(dataArray,materialId);
    if (materialURI==0) { arfSetError("textureSet.material (data id %d) does not resolve to a data item",materialId); return 0; }

    size_t size = 0;
    void  *bytes = arfExtractEntry(archive,materialURI,&size);
    if (bytes==0) { return 0; }

    avatar->textureSet.materialBytes  = bytes;   /* opaque, ownership transferred */
    avatar->textureSet.materialLength = size;
    snprintf(avatar->textureSet.materialMimeType,sizeof(avatar->textureSet.materialMimeType),
            "%s",materialType ? materialType : "");

    const struct arfJsonValue *targets = arfJsonMember(set,"targets");
    unsigned int count = arfJsonCount(targets);
    if (count==0) { arfSetError("textureSet.targets is empty or missing"); return 0; }

    avatar->textureSet.targets = (struct arfTextureTarget *) calloc(count,sizeof(struct arfTextureTarget));
    if (avatar->textureSet.targets==0) { arfSetError("out of memory allocating %u texture targets",count); return 0; }
    avatar->textureSet.numberOfTargets = count;

    for (unsigned int i=0; i<count; i++)
    {
        const struct arfJsonValue *target = arfJsonAt(targets,i);
        struct arfTextureTarget *destination = &avatar->textureSet.targets[i];

        const char *targetName = arfJsonString(arfJsonMember(target,"name"),0);
        if (targetName==0) { arfSetError("textureSet.targets[%u] has no name",i); return 0; }
        snprintf(destination->name,sizeof(destination->name),"%s",targetName);

        int textureId = (int) arfJsonNumber(arfJsonMember(target,"texture"),-1.0);
        const char *textureURI  = arfResolveDataURI(dataArray,textureId);
        const char *textureType = arfResolveDataType(dataArray,textureId);
        if (textureURI==0)
        {
            arfSetError("textureSet.targets[%u].texture (data id %d) does not resolve to a data item",i,textureId);
            return 0;
        }

        size_t tsize = 0;
        void  *tbytes = arfExtractEntry(archive,textureURI,&tsize);
        if (tbytes==0) { return 0; }

        destination->bytes  = tbytes;
        destination->length = tsize;
        snprintf(destination->mimeType,sizeof(destination->mimeType),"%s",textureType ? textureType : "");
    }

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

    arfCursorReadU32BE(&payload);  /* config timestamp, always zero */

    if (!arfCursorReadString8(&payload,profile,ARF_MAX_NAME))
    {
        arfSetError("%s: AAU_CONFIG profile string is truncated or too long",what);
        return 0;
    }

    *timescale = arfCursorReadF32BE(&payload);

    if (payload.failed)    { arfSetError("%s: AAU_CONFIG payload is truncated",what); return 0; }
    if (!(*timescale > 0)) { arfSetError("%s: AAU_CONFIG timescale is %f, expected a positive rate",what,*timescale); return 0; }

    return 1;
}

/** @brief Decode animations/joints.bin into the avatar's dense frame arrays.
 *  @param skeletonId the declared id of components.skeletons[0], checked
 *  against each frame's aja_joint_set_id. */
static int arfLoadJointStream(struct arfAvatar *avatar, const void *bytes, size_t size, int skeletonId)
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

        avatar->frameTimestamp[frame] = arfCursorReadU32BE(&payload);

        int frameSkeletonId = (int) arfCursorReadU16BE(&payload);
        if (frameSkeletonId != skeletonId)
        {
            arfSetError("frame %u targets joint set %d but the skeleton is %d",
                        frame,frameSkeletonId,skeletonId);
            return 0;
        }

        unsigned int flags           = arfCursorReadU8(&payload);
        int          velocityPresent = (flags >> 7) & 1;
        unsigned int jointCount      = arfCursorReadU16BE(&payload) + 1;

        float *matrices = avatar->localMatrices + (size_t) frame * matricesPerFrame;
        for (unsigned int j=0; j<avatar->numberOfNodes; j++) { arfIdentity4x4(matrices + (size_t) j*16); }

        for (unsigned int j=0; j<jointCount; j++)
        {
            unsigned int jointIndex = arfCursorReadU16BE(&payload);

            if (payload.failed) { break; }
            if (jointIndex >= avatar->numberOfNodes)
            {
                arfSetError("frame %u drives joint index %u but the skeleton has %u joints",
                            frame,jointIndex,avatar->numberOfNodes);
                return 0;
            }

            if (!arfCursorReadFloatsBE(&payload,matrices + (size_t) jointIndex*16,16)) { break; }

            if (velocityPresent)
            {
                /* Nothing in this library consumes joint velocity yet; read
                 * and discard it rather than storing it unused. */
                float discarded[16];
                if (!arfCursorReadFloatsBE(&payload,discarded,16)) { break; }
            }
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

/** @brief Decode animations/face.bin into the avatar's blendshape weight arrays.
 *  @param blendshapeSetId the declared id of components.blendshapeSets[0],
 *  checked against each frame's afa_blendshape_set_id. */
static int arfLoadFaceStream(struct arfAvatar *avatar, const void *bytes, size_t size, int blendshapeSetId)
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
        arfCursorInit(&payload,unit.payload,unit.length);

        avatar->faceTimestamp[frame] = arfCursorReadU32BE(&payload);

        int frameSetId = (int) arfCursorReadU16BE(&payload);
        if (frameSetId != blendshapeSetId)
        {
            arfSetError("face frame %u targets blendshape set %d but the blendshape set is %d",
                        frame,frameSetId,blendshapeSetId);
            return 0;
        }

        unsigned int flags             = arfCursorReadU8(&payload);
        int          confidencePresent = (flags >> 7) & 1;
        unsigned int entries           = arfCursorReadU16BE(&payload) + 1;

        float *weights = avatar->blendshapeWeights + (size_t) frame * shapes;
        for (unsigned int e=0; e<entries; e++)
        {
            unsigned int index = arfCursorReadU16BE(&payload);
            float        value = arfCursorReadF32BE(&payload);

            if (payload.failed)  { break; }

            if (confidencePresent)
            {
                /* Nothing in this library consumes blendshape confidence yet;
                 * read and discard it rather than storing it unused. */
                arfCursorReadF32BE(&payload);
                if (payload.failed) { break; }
            }

            if (index < shapes)  { weights[index] = value; }
        }

        if (payload.failed) { arfSetError("face frame %u is truncated",frame); return 0; }

        frame++;
    }

    return 1;
}

/** @brief Decode animations/landmarks.bin into the avatar's landmark position
 *  arrays.  Positions are always stored as 3 floats; a frame that arrives as
 *  2D (is3DFlag off) is stored with z=0, so the in-memory shape stays uniform.
 *  @param landmarkSetId the declared id of components.landmarkSets[0],
 *  checked against each frame's ala_landmark_set_id. */
static int arfLoadLandmarkStream(struct arfAvatar *avatar, const void *bytes, size_t size, int landmarkSetId)
{
    struct arfCursor cursor;
    char profile[ARF_MAX_NAME];

    arfCursorInit(&cursor,bytes,size);
    if (!arfReadStreamConfig(&cursor,profile,&avatar->landmarkTimescale,"the landmark animation stream")) { return 0; }

    struct arfCursor  counting = cursor;
    struct arfAAUUnit unit;
    unsigned int      frameCount = 0;

    while (arfAAUNext(&counting,&unit))
    {
        if (unit.type == ARF_AAU_LANDMARK) { frameCount++; }
    }

    if (counting.failed) { arfSetError("the landmark animation stream is truncated"); return 0; }
    if (frameCount==0)   { arfSetError("the landmark animation stream has no AAU_LANDMARK frames"); return 0; }

    unsigned int landmarks = avatar->landmarks.numberOfLandmarks;

    avatar->landmarkTimestamp = (unsigned int *) malloc((size_t) frameCount * sizeof(unsigned int));
    avatar->landmarkPositions = (float *)        calloc((size_t) frameCount * landmarks * 3,sizeof(float));

    if ( (avatar->landmarkTimestamp==0) || (avatar->landmarkPositions==0) )
    {
        arfSetError("out of memory allocating %u landmark frames",frameCount);
        return 0;
    }

    avatar->numberOfLandmarkFrames = frameCount;
    avatar->landmarkFrameCapacity  = frameCount;

    unsigned int frame = 0;
    while (arfAAUNext(&cursor,&unit))
    {
        if (unit.type != ARF_AAU_LANDMARK) { continue; }

        struct arfCursor payload;
        arfCursorInit(&payload,unit.payload,unit.length);

        avatar->landmarkTimestamp[frame] = arfCursorReadU32BE(&payload);

        int frameSetId = (int) arfCursorReadU16BE(&payload);
        if (frameSetId != landmarkSetId)
        {
            arfSetError("landmark frame %u targets landmark set %d but the landmark set is %d",
                        frame,frameSetId,landmarkSetId);
            return 0;
        }

        unsigned int flags             = arfCursorReadU8(&payload);
        int          velocityPresent   = (flags >> 7) & 1;
        int          confidencePresent = (flags >> 6) & 1;
        int          is3D              = (flags >> 5) & 1;
        unsigned int entries           = arfCursorReadU16BE(&payload) + 1;

        float *positions = avatar->landmarkPositions + (size_t) frame * landmarks * 3;
        for (unsigned int e=0; e<entries; e++)
        {
            unsigned int index = arfCursorReadU16BE(&payload);
            float        xyz[3] = {0.0f,0.0f,0.0f};

            if (!arfCursorReadFloatsBE(&payload,xyz,is3D ? 3 : 2)) { break; }

            if (velocityPresent)
            {
                /* Nothing in this library consumes landmark velocity yet;
                 * read and discard it rather than storing it unused. */
                arfCursorReadF32BE(&payload);
                if (payload.failed) { break; }
            }
            if (confidencePresent)
            {
                arfCursorReadF32BE(&payload);
                if (payload.failed) { break; }
            }

            if (index < landmarks) { memcpy(positions + (size_t) index*3,xyz,3*sizeof(float)); }
        }

        if (payload.failed) { arfSetError("landmark frame %u is truncated",frame); return 0; }

        frame++;
    }

    return 1;
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

    if (arfJsonCount(arfJsonMember(structure,"assets"))==0)
    {
        arfSetError("structure.assets is empty or missing");
        goto failed;
    }

    if (!arfLoadNodes(avatar,components))        { goto failed; }
    if (!arfValidateSkeleton(avatar,components)) { goto failed; }

    const struct arfJsonValue *mesh = arfJsonAt(arfJsonMember(components,"meshes"),0);
    if (mesh==0) { arfSetError("components.meshes is empty or missing"); goto failed; }

    const struct arfJsonValue *skin = arfJsonAt(arfJsonMember(components,"skins"),0);
    if (skin==0) { arfSetError("components.skins is empty or missing"); goto failed; }

    /* Only one mesh/skeleton is ever loaded, so "resolving" skin.mesh/
     * skin.skeleton is comparing two declared id values rather than a real
     * search -- but it must still be a value comparison, not an assumed 0,
     * since the spec resolves every reference by id, not by position. */
    const struct arfJsonValue *skeletonForCheck = arfJsonAt(arfJsonMember(components,"skeletons"),0);
    int meshDeclaredId     = (int) arfJsonNumber(arfJsonMember(mesh,"id"),-1.0);
    int skeletonDeclaredId = (int) arfJsonNumber(arfJsonMember(skeletonForCheck,"id"),-2.0);

    int skinMesh = (int) arfJsonNumber(arfJsonMember(skin,"mesh"),-1.0);
    if (skinMesh != meshDeclaredId)
    {
        arfSetError("skins[0].mesh is %d, expected %d (meshes[0].id) -- one mesh, one skin",skinMesh,meshDeclaredId);
        goto failed;
    }

    int skinSkeleton = (int) arfJsonNumber(arfJsonMember(skin,"skeleton"),-1.0);
    if (skinSkeleton != skeletonDeclaredId)
    {
        arfSetError("skins[0].skeleton is %d, expected %d (skeletons[0].id) -- one skeleton",skinSkeleton,skeletonDeclaredId);
        goto failed;
    }

    if (!arfLoadMesh(avatar,archive,mesh,dataArray))            { goto failed; }
    if (!arfLoadSkin(avatar,archive,components,skin,dataArray)) { goto failed; }

    /* Body animation.  The spec has no arf.json field naming this stream's
     * location for a Zip container; it is found by the fixed path the
     * container-format clause locates it at. */
    size_t streamSize = 0;
    streamBytes = arfExtractEntry(archive,ARF_ENTRY_JOINT_STREAM,&streamSize);
    if (streamBytes==0) { goto failed; }

    if (!arfLoadJointStream(avatar,streamBytes,streamSize,skeletonDeclaredId)) { goto failed; }
    free(streamBytes);
    streamBytes = 0;

    /* Face animation, if this container carries one. */
    const struct arfJsonValue *blendshapeSet = arfJsonAt(arfJsonMember(components,"blendshapeSets"),0);

    if (blendshapeSet!=0)
    {
        int blendshapeSetId = (int) arfJsonNumber(arfJsonMember(blendshapeSet,"id"),-3.0);
        int blendshapeBaseMesh = (int) arfJsonNumber(arfJsonMember(blendshapeSet,"baseMesh"),-1.0);
        if (blendshapeBaseMesh != meshDeclaredId)
        {
            arfSetError("blendshapeSets[0].baseMesh is %d, expected %d (meshes[0].id)",blendshapeBaseMesh,meshDeclaredId);
            goto failed;
        }

        if (!arfLoadBlendshapes(avatar,archive,blendshapeSet,dataArray)) { goto failed; }

        streamBytes = arfExtractEntry(archive,ARF_ENTRY_FACE_STREAM,&streamSize);
        if (streamBytes==0) { goto failed; }

        if (!arfLoadFaceStream(avatar,streamBytes,streamSize,blendshapeSetId)) { goto failed; }
        free(streamBytes);
        streamBytes = 0;

        avatar->hasFace = 1;
    }

    /* Landmark animation, if this container carries one. */
    const struct arfJsonValue *landmarkSet = arfJsonAt(arfJsonMember(components,"landmarkSets"),0);

    if (landmarkSet!=0)
    {
        int landmarkSetId = (int) arfJsonNumber(arfJsonMember(landmarkSet,"id"),-4.0);
        int landmarkBaseMesh = (int) arfJsonNumber(arfJsonMember(landmarkSet,"baseMesh"),-1.0);
        if (landmarkBaseMesh != meshDeclaredId)
        {
            arfSetError("landmarkSets[0].baseMesh is %d, expected %d (meshes[0].id)",landmarkBaseMesh,meshDeclaredId);
            goto failed;
        }

        if (!arfLoadLandmarks(avatar,archive,landmarkSet,dataArray)) { goto failed; }

        streamBytes = arfExtractEntry(archive,ARF_ENTRY_LANDMARK_STREAM,&streamSize);
        if (streamBytes==0) { goto failed; }

        if (!arfLoadLandmarkStream(avatar,streamBytes,streamSize,landmarkSetId)) { goto failed; }
        free(streamBytes);
        streamBytes = 0;

        avatar->hasLandmarks = 1;
    }

    /* Texture set, if this container carries one.  TextureSet has no
     * baseMesh/mesh field of its own -- skins[0].textureSet is the only
     * link tying it to anything, so that's what gets cross-checked here,
     * the reverse direction from the mesh/skeleton/blendshapeSet checks
     * above. */
    const struct arfJsonValue *textureSet = arfJsonAt(arfJsonMember(components,"textureSets"),0);

    if (textureSet!=0)
    {
        int textureSetId = (int) arfJsonNumber(arfJsonMember(textureSet,"id"),-5.0);
        int skinTextureSet = (int) arfJsonNumber(arfJsonMember(skin,"textureSet"),-1.0);
        if (skinTextureSet != textureSetId)
        {
            arfSetError("skins[0].textureSet is %d, expected %d (textureSets[0].id)",skinTextureSet,textureSetId);
            goto failed;
        }

        if (!arfLoadTextureSet(avatar,archive,textureSet,dataArray)) { goto failed; }

        avatar->hasTextureSet = 1;
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
    free(avatar->landmarks.vertexIndex);
    free(avatar->landmarkTimestamp);
    free(avatar->landmarkPositions);
    free(avatar->textureSet.materialBytes);
    for (unsigned int i=0; i<avatar->textureSet.numberOfTargets; i++)
    {
        free(avatar->textureSet.targets[i].bytes);
    }
    free(avatar->textureSet.targets);
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
    printf("skeleton      : %u nodes, root=\"%s\"\n",avatar->numberOfNodes,avatar->nodes[avatar->rootNode].name);
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
               avatar->nodes[root].name,
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

    if (avatar->hasLandmarks)
    {
        printf("landmark track: %u landmarks, %u frames at %.3f ticks/s\n",
               avatar->landmarks.numberOfLandmarks,avatar->numberOfLandmarkFrames,avatar->landmarkTimescale);
    }
    else
    {
        printf("landmark track: absent\n");
    }

    if (avatar->hasTextureSet)
    {
        printf("texture set   : \"%s\", material %zu bytes (%s), %u target(s)\n",
               avatar->textureSet.name,avatar->textureSet.materialLength,
               avatar->textureSet.materialMimeType,avatar->textureSet.numberOfTargets);
    }
    else
    {
        printf("texture set   : absent\n");
    }
}
