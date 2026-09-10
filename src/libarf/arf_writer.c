/** @file arf_writer.c
 *  @brief Builds and serialises a .arfz container.
 *
 *  The order of work is the reverse of the reader's: encode the binary tensors
 *  and animation streams first, then write an arf.json whose data[] byteLength
 *  fields are the sizes those buffers actually came out as.  Deriving the JSON
 *  from the bytes rather than the other way round is what makes a container
 *  self-consistent by construction -- the reader checks exactly this
 *  correspondence and rejects a mismatch.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf.h"
#include "arf_bytes.h"
#include "arf_internal.h"

#include <zip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ===========================================================================
 *  Building an avatar in memory
 * =========================================================================*/

struct arfAvatar *arfCreate(unsigned int numberOfNodes,
                            unsigned int numberOfVertices,
                            unsigned int numberOfTriangles,
                            unsigned int numberOfWeights)
{
    if ( (numberOfNodes==0) || (numberOfVertices==0) || (numberOfTriangles==0) )
    {
        arfSetError("an avatar needs at least one node, one vertex and one triangle");
        return 0;
    }

    struct arfAvatar *avatar = (struct arfAvatar *) calloc(1,sizeof(struct arfAvatar));
    if (avatar==0) { arfSetError("out of memory"); return 0; }

    snprintf(avatar->name,sizeof(avatar->name),"%s","avatar");
    snprintf(avatar->id,sizeof(avatar->id),"%s","0");

    avatar->numberOfNodes = numberOfNodes;
    avatar->nodes = (struct arfNode *) calloc(numberOfNodes,sizeof(struct arfNode));

    avatar->mesh.numberOfVertices  = numberOfVertices;
    avatar->mesh.numberOfTriangles = numberOfTriangles;
    avatar->mesh.positions = (float *)        calloc((size_t) numberOfVertices * 3,sizeof(float));
    avatar->mesh.indices   = (unsigned int *) calloc((size_t) numberOfTriangles * 3,sizeof(unsigned int));

    avatar->skin.numberOfVertices = numberOfVertices;
    avatar->skin.numberOfJoints   = numberOfNodes;
    avatar->skin.numberOfWeights  = numberOfWeights;
    avatar->skin.vertexIndex = (unsigned int *) calloc((size_t) numberOfWeights + 1,sizeof(unsigned int));
    avatar->skin.jointIndex  = (unsigned int *) calloc((size_t) numberOfWeights + 1,sizeof(unsigned int));
    avatar->skin.weight      = (float *)        calloc((size_t) numberOfWeights + 1,sizeof(float));
    avatar->skin.inverseBindMatrices = (float *) calloc((size_t) numberOfNodes * 16,sizeof(float));

    avatar->timescale = 30.0f;

    if ( (avatar->nodes==0) || (avatar->mesh.positions==0) || (avatar->mesh.indices==0) ||
         (avatar->skin.vertexIndex==0) || (avatar->skin.jointIndex==0) ||
         (avatar->skin.weight==0) || (avatar->skin.inverseBindMatrices==0) )
    {
        arfFree(avatar);
        arfSetError("out of memory allocating an avatar of %u nodes and %u vertices",numberOfNodes,numberOfVertices);
        return 0;
    }

    for (unsigned int j=0; j<numberOfNodes; j++)
    {
        avatar->nodes[j].parent = (j==0) ? -1 : 0;
        avatar->nodes[j].rotation[3] = 1.0f;
        avatar->nodes[j].scale[0] = avatar->nodes[j].scale[1] = avatar->nodes[j].scale[2] = 1.0f;
        snprintf(avatar->nodes[j].name,ARF_MAX_NAME,"joint%u",j);
        arfIdentity4x4(avatar->skin.inverseBindMatrices + (size_t) j * 16);
    }

    return avatar;
}

int arfSetNode(struct arfAvatar *avatar, unsigned int index, const char *name, int parent,
               const float *translation, const float *rotation)
{
    if ( (avatar==0) || (name==0) )        { arfSetError("no avatar or node name was given"); return ARF_ERROR_ARGUMENT; }
    if (index >= avatar->numberOfNodes)    { arfSetError("node index %u is past the %u node skeleton",index,avatar->numberOfNodes); return ARF_ERROR_ARGUMENT; }
    if (strlen(name) >= ARF_MAX_NAME)      { arfSetError("node name \"%s\" is longer than the %d character limit",name,ARF_MAX_NAME-1); return ARF_ERROR_ARGUMENT; }

    /* Parents must precede their children, both because the format's readers
     * compose the hierarchy in a single forward pass and because it is the
     * only cheap guarantee against a cycle. */
    if ( (parent >= (int) index) || (parent < -1) )
    {
        arfSetError("node %u \"%s\" cannot have parent %d -- parents must be listed before their children",index,name,parent);
        return ARF_ERROR_ARGUMENT;
    }

    struct arfNode *node = &avatar->nodes[index];
    strcpy(node->name,name);
    node->parent = parent;

    if (parent < 0) { avatar->rootNode = index; }

    if (translation!=0) { memcpy(node->translation,translation,3*sizeof(float)); }
    if (rotation!=0)    { memcpy(node->rotation,rotation,4*sizeof(float));       }

    return ARF_OK;
}

/** @brief Grow an array in place to hold at least `capacity` entries.
 *  @retval 1 on success, 0 if the reallocation failed */
static int arfGrow(void **array, unsigned int capacity, size_t entrySize)
{
    void *grown = realloc(*array,(size_t) capacity * entrySize);
    if (grown==0) { return 0; }

    *array = grown;
    return 1;
}

/** @brief Next capacity that holds `wanted` entries, doubling from `capacity`. */
static unsigned int arfNextCapacity(unsigned int capacity, unsigned int wanted)
{
    unsigned int grown = (capacity==0) ? 64 : capacity;
    while (grown < wanted) { grown *= 2; }
    return grown;
}

int arfAppendFrame(struct arfAvatar *avatar, unsigned int timestamp, const float *localMatrices)
{
    if ( (avatar==0) || (localMatrices==0) ) { arfSetError("no avatar or matrices were given"); return ARF_ERROR_ARGUMENT; }

    unsigned int wanted = avatar->numberOfFrames + 1;

    if (wanted > avatar->frameCapacity)
    {
        unsigned int capacity = arfNextCapacity(avatar->frameCapacity,wanted);

        if (!arfGrow((void **) &avatar->frameTimestamp,capacity,sizeof(unsigned int)) ||
            !arfGrow((void **) &avatar->localMatrices,capacity,(size_t) avatar->numberOfNodes * 16 * sizeof(float)))
        {
            arfSetError("out of memory growing the body track to %u frames",capacity);
            return ARF_ERROR_MEMORY;
        }

        avatar->frameCapacity = capacity;
    }

    avatar->frameTimestamp[avatar->numberOfFrames] = timestamp;
    memcpy(avatar->localMatrices + (size_t) avatar->numberOfFrames * avatar->numberOfNodes * 16,
           localMatrices,(size_t) avatar->numberOfNodes * 16 * sizeof(float));

    avatar->numberOfFrames++;
    return ARF_OK;
}

int arfEnableFace(struct arfAvatar *avatar, unsigned int numberOfShapes, const float *deltas)
{
    if ( (avatar==0) || (deltas==0) || (numberOfShapes==0) ) { arfSetError("no avatar, shape count or deltas were given"); return ARF_ERROR_ARGUMENT; }

    size_t count = (size_t) numberOfShapes * avatar->mesh.numberOfVertices * 3;

    free(avatar->blendshapes.deltas);
    avatar->blendshapes.deltas = (float *) malloc(count * sizeof(float));
    if (avatar->blendshapes.deltas==0) { arfSetError("out of memory allocating %u blendshape targets",numberOfShapes); return ARF_ERROR_MEMORY; }

    memcpy(avatar->blendshapes.deltas,deltas,count * sizeof(float));
    avatar->blendshapes.numberOfShapes   = numberOfShapes;
    avatar->blendshapes.numberOfVertices = avatar->mesh.numberOfVertices;
    avatar->faceTimescale = avatar->timescale;
    avatar->hasFace = 1;

    return ARF_OK;
}

int arfAppendFaceFrame(struct arfAvatar *avatar, unsigned int timestamp, const float *weights)
{
    if ( (avatar==0) || (weights==0) )  { arfSetError("no avatar or weights were given"); return ARF_ERROR_ARGUMENT; }
    if (!avatar->hasFace)               { arfSetError("call arfEnableFace() before appending face frames"); return ARF_ERROR_ARGUMENT; }

    unsigned int wanted = avatar->numberOfFaceFrames + 1;

    if (wanted > avatar->faceFrameCapacity)
    {
        unsigned int capacity = arfNextCapacity(avatar->faceFrameCapacity,wanted);

        if (!arfGrow((void **) &avatar->faceTimestamp,capacity,sizeof(unsigned int)) ||
            !arfGrow((void **) &avatar->blendshapeWeights,capacity,
                     (size_t) avatar->blendshapes.numberOfShapes * sizeof(float)))
        {
            arfSetError("out of memory growing the face track to %u frames",capacity);
            return ARF_ERROR_MEMORY;
        }

        avatar->faceFrameCapacity = capacity;
    }

    avatar->faceTimestamp[avatar->numberOfFaceFrames] = timestamp;
    memcpy(avatar->blendshapeWeights + (size_t) avatar->numberOfFaceFrames * avatar->blendshapes.numberOfShapes,
           weights,(size_t) avatar->blendshapes.numberOfShapes * sizeof(float));

    avatar->numberOfFaceFrames++;
    return ARF_OK;
}


/* ===========================================================================
 *  Tensor and stream encoding
 * =========================================================================*/

static void arfWriteDenseHeader(struct arfBuffer *buffer, const int *dims, int numberOfDimensions, int dtype)
{
    arfBufferWriteI32(buffer,numberOfDimensions);
    for (int i=0; i<numberOfDimensions; i++) { arfBufferWriteI32(buffer,dims[i]); }
    arfBufferWriteI32(buffer,dtype);
}

static void arfWriteDenseFloats(struct arfBuffer *buffer, const int *dims, int numberOfDimensions, const float *values)
{
    size_t count = 1;
    for (int i=0; i<numberOfDimensions; i++) { count *= (size_t) dims[i]; }

    arfWriteDenseHeader(buffer,dims,numberOfDimensions,ARF_COMPONENT_FLOAT);
    arfBufferWriteFloats(buffer,values,count);
}

static void arfWriteSparseWeights(struct arfBuffer *buffer, const struct arfSkin *skin)
{
    arfBufferWriteI32(buffer,2);
    arfBufferWriteI32(buffer,(int) skin->numberOfVertices);
    arfBufferWriteI32(buffer,(int) skin->numberOfJoints);
    arfBufferWriteI32(buffer,(int) skin->numberOfWeights);
    arfBufferWriteI32(buffer,ARF_COMPONENT_UNSIGNED_INT);
    arfBufferWriteI32(buffer,ARF_COMPONENT_FLOAT);

    /* Back to the flat row-major index the format stores. */
    for (unsigned int e=0; e<skin->numberOfWeights; e++)
    {
        arfBufferWriteU32(buffer,skin->vertexIndex[e] * skin->numberOfJoints + skin->jointIndex[e]);
    }

    arfBufferWriteFloats(buffer,skin->weight,skin->numberOfWeights);
}

/** @brief Frame one AAU: (type<<1)|reserved byte, big-endian payload length,
 *  payload -- see the "Avatar Animation Unit framing" note in arf_format.h. */
static void arfAppendAAU(struct arfBuffer *stream, unsigned int type, const struct arfBuffer *payload)
{
    arfBufferWriteU8(stream,type << 1);
    arfBufferWriteU32BE(stream,(unsigned int) payload->length);
    arfBufferAppend(stream,payload->data,payload->length);
}

static void arfWriteStreamConfig(struct arfBuffer *stream, const char *profile, float timescale)
{
    struct arfBuffer payload;
    arfBufferInit(&payload);

    arfBufferWriteU32BE(&payload,0);          /* config units are always at timestamp zero */
    arfBufferWriteString8(&payload,profile);
    arfBufferWriteF32BE(&payload,timescale);

    arfAppendAAU(stream,ARF_AAU_CONFIG,&payload);
    arfBufferFree(&payload);
}

/** @param skeletonId the declared id of components.skeletons[0], written as
 *  each frame's aja_joint_set_id. */
static void arfWriteJointStream(struct arfBuffer *stream, const struct arfAvatar *avatar, int skeletonId)
{
    arfWriteStreamConfig(stream,ARF_PROFILE_BODY,avatar->timescale);

    struct arfBuffer payload;
    arfBufferInit(&payload);

    for (unsigned int f=0; f<avatar->numberOfFrames; f++)
    {
        payload.length = 0;
        payload.failed = 0;

        arfBufferWriteU32BE(&payload,avatar->frameTimestamp[f]);
        arfBufferWriteU16BE(&payload,(unsigned int) skeletonId);
        arfBufferWriteU8(&payload,0);                              /* velocityPresent=0, reserved=0 */
        arfBufferWriteU16BE(&payload,avatar->numberOfNodes - 1);   /* jointCountMinus1 */

        const float *matrices = avatar->localMatrices + (size_t) f * avatar->numberOfNodes * 16;
        for (unsigned int j=0; j<avatar->numberOfNodes; j++)
        {
            arfBufferWriteU16BE(&payload,j);
            arfBufferWriteFloatsBE(&payload,matrices + (size_t) j * 16,16);
        }

        arfAppendAAU(stream,ARF_AAU_JOINT,&payload);
    }

    arfBufferFree(&payload);
}

/** @param blendshapeSetId the declared id of components.blendshapeSets[0],
 *  written as each frame's afa_blendshape_set_id. */
static void arfWriteFaceStream(struct arfBuffer *stream, const struct arfAvatar *avatar, int blendshapeSetId)
{
    arfWriteStreamConfig(stream,ARF_PROFILE_FACE,avatar->faceTimescale);

    struct arfBuffer payload;
    arfBufferInit(&payload);

    for (unsigned int f=0; f<avatar->numberOfFaceFrames; f++)
    {
        payload.length = 0;
        payload.failed = 0;

        arfBufferWriteU32BE(&payload,avatar->faceTimestamp[f]);
        arfBufferWriteU16BE(&payload,(unsigned int) blendshapeSetId);
        arfBufferWriteU8(&payload,0);                                        /* confidencePresent=0, reserved=0 */
        arfBufferWriteU16BE(&payload,avatar->blendshapes.numberOfShapes - 1); /* blendshapeCountMinus1 */

        const float *weights = avatar->blendshapeWeights + (size_t) f * avatar->blendshapes.numberOfShapes;
        for (unsigned int s=0; s<avatar->blendshapes.numberOfShapes; s++)
        {
            arfBufferWriteU16BE(&payload,s);
            arfBufferWriteF32BE(&payload,weights[s]);
        }

        arfAppendAAU(stream,ARF_AAU_BLENDSHAPE,&payload);
    }

    arfBufferFree(&payload);
}


/* ===========================================================================
 *  arf.json
 * =========================================================================*/

static void arfJsonText(struct arfBuffer *buffer, const char *text)
{
    arfBufferAppend(buffer,text,strlen(text));
}

/** @brief Emit a JSON string literal, escaping what RFC 8259 requires. */
static void arfJsonQuoted(struct arfBuffer *buffer, const char *text)
{
    arfBufferWriteU8(buffer,'"');

    for (const unsigned char *at=(const unsigned char *) text; *at!=0; at++)
    {
        switch (*at)
        {
            case '"'  : arfJsonText(buffer,"\\\""); break;
            case '\\' : arfJsonText(buffer,"\\\\"); break;
            case '\b' : arfJsonText(buffer,"\\b");  break;
            case '\f' : arfJsonText(buffer,"\\f");  break;
            case '\n' : arfJsonText(buffer,"\\n");  break;
            case '\r' : arfJsonText(buffer,"\\r");  break;
            case '\t' : arfJsonText(buffer,"\\t");  break;
            default:
                if (*at < 0x20)
                {
                    char escape[8];
                    snprintf(escape,sizeof(escape),"\\u%04x",*at);
                    arfJsonText(buffer,escape);
                }
                else { arfBufferWriteU8(buffer,*at); }
            break;
        }
    }

    arfBufferWriteU8(buffer,'"');
}

/** @brief Emit a float.  %.9g is the shortest form that always round-trips a
 *  binary32, so re-reading a container never shifts a rest offset. */
static void arfJsonFloat(struct arfBuffer *buffer, float value)
{
    char text[32];
    snprintf(text,sizeof(text),"%.9g",(double) value);
    arfJsonText(buffer,text);
}

static void arfJsonUnsigned(struct arfBuffer *buffer, unsigned long long value)
{
    char text[32];
    snprintf(text,sizeof(text),"%llu",value);
    arfJsonText(buffer,text);
}

/** @brief One entry of the data[] array.  @param id the data item's numeric
 *  id (its position in data[], this library's own numbering convention). */
static void arfJsonDataItem(struct arfBuffer *buffer, int id, const char *name, const char *uri,
                            const char *type, size_t byteLength, int isLast)
{
    arfJsonText(buffer,"    {\"id\": ");
    arfJsonUnsigned(buffer,(unsigned long long) id);
    arfJsonText(buffer,", \"name\": ");
    arfJsonQuoted(buffer,name);
    arfJsonText(buffer,", \"uri\": ");
    arfJsonQuoted(buffer,uri);
    arfJsonText(buffer,", \"type\": ");
    arfJsonQuoted(buffer,type);
    arfJsonText(buffer,", \"byteLength\": ");
    arfJsonUnsigned(buffer,(unsigned long long) byteLength);
    arfJsonText(buffer,isLast ? "}\n" : "},\n");
}

/** @brief Serialise the whole base avatar model document.
 *
 *  The byteLength fields come from the encoded buffers rather than from any
 *  recomputed size, which is what keeps the document and the binaries in
 *  agreement.
 *
 *  Every component's numeric id is its index within its own
 *  components.<array> -- this library only ever holds one mesh/skin/
 *  skeleton/blendshapeSet, so those always get id 0.  data[] ids are defined
 *  in arf_format.h (ARF_DATA_ID_*). */
static void arfWriteJson(struct arfBuffer *buffer, const struct arfAvatar *avatar,
                         size_t positionsBytes, size_t indicesBytes, size_t weightsBytes,
                         size_t inverseBindBytes, size_t deltaBytes)
{
    arfJsonText(buffer,"{\n");

    arfJsonText(buffer,"  \"preamble\": {\"signature\": \"" ARF_SIGNATURE "\", \"version\": \"" ARF_CONTAINER_VERSION
                       "\", \"supportedAnimations\": [\"" ARF_PROFILE_BODY "\"");
    if (avatar->hasFace) { arfJsonText(buffer,", \"" ARF_PROFILE_FACE "\""); }
    arfJsonText(buffer,"]},\n");

    arfJsonText(buffer,"  \"metadata\": {\"name\": ");
    arfJsonQuoted(buffer,avatar->name);
    arfJsonText(buffer,", \"id\": ");
    arfJsonQuoted(buffer,avatar->id);
    arfJsonText(buffer,"},\n");

    /* structure.assets[].lods[] replaces the invented animationStreams field --
     * the spec has no field naming the animation streams, they are located by
     * the fixed paths in arf_format.h per the Zip-container clause. */
    arfJsonText(buffer,"  \"structure\": {\"assets\": [{\"name\": \"body\", \"isMain\": true, \"lods\": [\n");
    arfJsonText(buffer,"    {\"name\": \"lod0\", \"skins\": [0], \"meshes\": [0], \"skeletons\": [0]");
    if (avatar->hasFace) { arfJsonText(buffer,", \"blendshapeSets\": [0]"); }
    arfJsonText(buffer,"}\n  ]}]},\n");

    arfJsonText(buffer,"  \"components\": {\n");

    arfJsonText(buffer,"    \"nodes\": [\n");
    for (unsigned int j=0; j<avatar->numberOfNodes; j++)
    {
        const struct arfNode *node = &avatar->nodes[j];

        arfJsonText(buffer,"      {\"id\": ");
        arfJsonUnsigned(buffer,j);
        arfJsonText(buffer,", \"name\": ");
        arfJsonQuoted(buffer,node->name);

        /* mapping is a mandatory semantic path in the spec; this project has
         * no access to a verified taxonomy for it (that lives in the scene
         * description part, 23090-14), so the node's own name is used as an
         * honest single-segment placeholder rather than a fabricated one. */
        arfJsonText(buffer,", \"mapping\": ");
        arfJsonQuoted(buffer,node->name);

        if (node->parent >= 0)
        {
            arfJsonText(buffer,", \"parent\": ");
            arfJsonUnsigned(buffer,(unsigned int) node->parent);
        }

        arfJsonText(buffer,", \"translation\": [");
        for (unsigned int c=0; c<3; c++)
        {
            if (c>0) { arfJsonText(buffer,", "); }
            arfJsonFloat(buffer,node->translation[c]);
        }

        arfJsonText(buffer,"], \"rotation\": [");
        for (unsigned int c=0; c<4; c++)
        {
            if (c>0) { arfJsonText(buffer,", "); }
            arfJsonFloat(buffer,node->rotation[c]);
        }

        arfJsonText(buffer,"], \"scale\": [");
        for (unsigned int c=0; c<3; c++)
        {
            if (c>0) { arfJsonText(buffer,", "); }
            arfJsonFloat(buffer,node->scale[c]);
        }

        arfJsonText(buffer,(j+1 < avatar->numberOfNodes) ? "]},\n" : "]}\n");
    }
    arfJsonText(buffer,"    ],\n");

    arfJsonText(buffer,"    \"skeletons\": [{\"id\": 0, \"name\": \"skeleton0\", \"root\": ");
    arfJsonUnsigned(buffer,avatar->rootNode);
    arfJsonText(buffer,", \"joints\": [");
    for (unsigned int j=0; j<avatar->numberOfNodes; j++)
    {
        if (j>0) { arfJsonText(buffer,", "); }
        arfJsonUnsigned(buffer,j);
    }
    arfJsonText(buffer,"], \"inverseBindMatrix\": ");
    arfJsonUnsigned(buffer,ARF_DATA_ID_INVERSE_BIND);
    arfJsonText(buffer,"}],\n");

    arfJsonText(buffer,"    \"skins\": [{\"id\": 0, \"name\": \"skin0\", \"mapping\": \"skin0\", "
                       "\"skeleton\": 0, \"mesh\": 0, \"weights\": ");
    arfJsonUnsigned(buffer,ARF_DATA_ID_SKIN_WEIGHTS);
    arfJsonText(buffer,"}],\n");

    arfJsonText(buffer,"    \"meshes\": [{\"id\": 0, \"name\": \"mesh0\", \"path\": \"mesh0\", \"data\": [");
    arfJsonUnsigned(buffer,ARF_DATA_ID_MESH_POSITIONS);
    arfJsonText(buffer,", ");
    arfJsonUnsigned(buffer,ARF_DATA_ID_MESH_INDICES);
    arfJsonText(buffer,"]}]");

    if (avatar->hasFace)
    {
        arfJsonText(buffer,",\n    \"blendshapeSets\": [{\"id\": 0, \"name\": \"" ARF_BLENDSHAPE_SET_ID
                           "\", \"baseMesh\": 0, \"shapes\": [");
        arfJsonUnsigned(buffer,ARF_DATA_ID_FACE_DELTAS);
        arfJsonText(buffer,"]}]");
    }

    arfJsonText(buffer,"\n  },\n");

    arfJsonText(buffer,"  \"data\": [\n");
    arfJsonDataItem(buffer,ARF_DATA_ID_MESH_POSITIONS,ARF_ID_MESH_POSITIONS,ARF_ENTRY_MESH_POSITIONS,ARF_MIME_DENSE, positionsBytes,  0);
    arfJsonDataItem(buffer,ARF_DATA_ID_MESH_INDICES,  ARF_ID_MESH_INDICES,  ARF_ENTRY_MESH_INDICES,  ARF_MIME_DENSE, indicesBytes,    0);
    arfJsonDataItem(buffer,ARF_DATA_ID_SKIN_WEIGHTS,  ARF_ID_SKIN_WEIGHTS,  ARF_ENTRY_SKIN_WEIGHTS,  ARF_MIME_SPARSE,weightsBytes,    0);
    arfJsonDataItem(buffer,ARF_DATA_ID_INVERSE_BIND,  ARF_ID_INVERSE_BIND,  ARF_ENTRY_INV_BIND_POSE, ARF_MIME_DENSE, inverseBindBytes,!avatar->hasFace);

    if (avatar->hasFace)
    {
        arfJsonDataItem(buffer,ARF_DATA_ID_FACE_DELTAS,ARF_ID_FACE_DELTAS,ARF_ENTRY_FACE_DELTAS,ARF_MIME_DENSE,deltaBytes,1);
    }

    arfJsonText(buffer,"  ]\n}\n");
}

/** @brief Build the non-normative id_map.txt sidecar -- see arf_format.h. */
static void arfWriteIdMap(struct arfBuffer *buffer, const struct arfAvatar *avatar)
{
    char line[128];

    for (unsigned int j=0; j<avatar->numberOfNodes; j++)
    {
        snprintf(line,sizeof(line),"node\t%u\t%s\n",j,avatar->nodes[j].name);
        arfBufferAppend(buffer,line,strlen(line));
    }

    arfBufferAppend(buffer,"mesh\t0\tmesh0\n",13);
    arfBufferAppend(buffer,"skin\t0\tskin0\n",13);
    arfBufferAppend(buffer,"skeleton\t0\tskeleton0\n",21);

    if (avatar->hasFace)
    {
        arfJsonText(buffer,"blendshapeSet\t0\t" ARF_BLENDSHAPE_SET_ID "\n");
    }
}


/* ===========================================================================
 *  arfSave
 * =========================================================================*/

/** @brief Add one buffer to the archive.
 *
 *  The buffer is referenced, not copied -- libzip reads it when the archive is
 *  closed -- so every arfBuffer here has to stay alive until zip_close().
 *
 *  Compression level 1 matches the producing pipeline's MZ_BEST_SPEED: these
 *  payloads are float tensors that barely compress, and a container is written
 *  once per tracked person inside a real-time loop. */
static int arfAddEntry(zip_t *archive, const char *name, const struct arfBuffer *buffer)
{
    if (buffer->failed) { arfSetError("ran out of memory encoding \"%s\"",name); return 0; }

    zip_source_t *source = zip_source_buffer(archive,buffer->data,buffer->length,0);
    if (source==0)
    {
        arfSetError("could not stage \"%s\": %s",name,zip_strerror(archive));
        return 0;
    }

    zip_int64_t index = zip_file_add(archive,name,source,ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (index < 0)
    {
        arfSetError("could not add \"%s\" to the container: %s",name,zip_strerror(archive));
        zip_source_free(source);
        return 0;
    }

    if (zip_set_file_compression(archive,(zip_uint64_t) index,ZIP_CM_DEFLATE,1) != 0)
    {
        arfSetError("could not set compression on \"%s\": %s",name,zip_strerror(archive));
        return 0;
    }

    return 1;
}

int arfSave(const struct arfAvatar *avatar, const char *filename)
{
    if ( (avatar==0) || (filename==0) )    { arfSetError("no avatar or filename was given"); return ARF_ERROR_ARGUMENT; }
    if (avatar->numberOfFrames==0)         { arfSetError("refusing to write a container with no animation frames"); return ARF_ERROR_ARGUMENT; }

    struct arfBuffer positions, indices, weights, inverseBind, deltas, jointStream, faceStream, json, idMap;
    arfBufferInit(&positions);
    arfBufferInit(&indices);
    arfBufferInit(&weights);
    arfBufferInit(&inverseBind);
    arfBufferInit(&deltas);
    arfBufferInit(&jointStream);
    arfBufferInit(&faceStream);
    arfBufferInit(&json);
    arfBufferInit(&idMap);

    int    result  = ARF_ERROR_IO;
    zip_t *archive = 0;

    int positionDims[2] = { (int) avatar->mesh.numberOfVertices, 3 };
    arfWriteDenseFloats(&positions,positionDims,2,avatar->mesh.positions);

    int indexDims[2] = { (int) avatar->mesh.numberOfTriangles, 3 };
    arfWriteDenseHeader(&indices,indexDims,2,ARF_COMPONENT_UNSIGNED_INT);
    arfBufferWriteU32s(&indices,avatar->mesh.indices,(size_t) avatar->mesh.numberOfTriangles * 3);

    arfWriteSparseWeights(&weights,&avatar->skin);

    int inverseBindDims[2] = { (int) avatar->numberOfNodes, 16 };
    arfWriteDenseFloats(&inverseBind,inverseBindDims,2,avatar->skin.inverseBindMatrices);

    /* 0 in both calls below: arfWriteJson() always gives the skeleton and
     * the blendshape set id 0, so the AAU streams target the same ids. */
    arfWriteJointStream(&jointStream,avatar,0);

    if (avatar->hasFace)
    {
        int deltaDims[3] = { (int) avatar->blendshapes.numberOfShapes,
                             (int) avatar->blendshapes.numberOfVertices, 3 };
        arfWriteDenseFloats(&deltas,deltaDims,3,avatar->blendshapes.deltas);
        arfWriteFaceStream(&faceStream,avatar,0);
    }

    arfWriteJson(&json,avatar,positions.length,indices.length,weights.length,inverseBind.length,deltas.length);
    arfWriteIdMap(&idMap,avatar);

    int openError = 0;
    archive = zip_open(filename,ZIP_CREATE | ZIP_TRUNCATE,&openError);
    if (archive==0)
    {
        zip_error_t error;
        zip_error_init_with_code(&error,openError);
        arfSetError("cannot create \"%s\": %s",filename,zip_error_strerror(&error));
        zip_error_fini(&error);
        goto cleanup;
    }

    if (!arfAddEntry(archive,ARF_ENTRY_JSON,&json))                  { goto cleanup; }
    if (!arfAddEntry(archive,ARF_ENTRY_ID_MAP,&idMap))               { goto cleanup; }
    if (!arfAddEntry(archive,ARF_ENTRY_MESH_POSITIONS,&positions))   { goto cleanup; }
    if (!arfAddEntry(archive,ARF_ENTRY_MESH_INDICES,&indices))       { goto cleanup; }
    if (!arfAddEntry(archive,ARF_ENTRY_SKIN_WEIGHTS,&weights))       { goto cleanup; }
    if (!arfAddEntry(archive,ARF_ENTRY_INV_BIND_POSE,&inverseBind))  { goto cleanup; }
    if (!arfAddEntry(archive,ARF_ENTRY_JOINT_STREAM,&jointStream))   { goto cleanup; }

    if (avatar->hasFace)
    {
        if (!arfAddEntry(archive,ARF_ENTRY_FACE_DELTAS,&deltas))     { goto cleanup; }
        if (!arfAddEntry(archive,ARF_ENTRY_FACE_STREAM,&faceStream)) { goto cleanup; }
    }

    /* Everything is written here, reading the staged buffers, so this has to
     * happen before the buffers below are freed. */
    if (zip_close(archive) != 0)
    {
        arfSetError("could not finalize \"%s\": %s",filename,zip_strerror(archive));
        goto cleanup;
    }
    archive = 0;

    result = ARF_OK;

cleanup:
    /* zip_discard drops a half-built archive without touching the staged
     * sources; zip_close already released them on the success path. */
    if (archive!=0) { zip_discard(archive); }

    arfBufferFree(&positions);
    arfBufferFree(&indices);
    arfBufferFree(&weights);
    arfBufferFree(&inverseBind);
    arfBufferFree(&deltas);
    arfBufferFree(&jointStream);
    arfBufferFree(&faceStream);
    arfBufferFree(&json);
    arfBufferFree(&idMap);

    return result;
}

int arfExportOBJ(const struct arfAvatar *avatar, const float *positions, const char *filename)
{
    if ( (avatar==0) || (filename==0) ) { arfSetError("no avatar or filename was given"); return ARF_ERROR_ARGUMENT; }

    if (positions==0) { positions = avatar->mesh.positions; }

    FILE *file = fopen(filename,"w");
    if (file==0) { arfSetError("cannot create \"%s\"",filename); return ARF_ERROR_IO; }

    fprintf(file,"# %s, exported by libarf %s -- units are centimetres\n",avatar->name,arfLibraryVersion);

    for (unsigned int v=0; v<avatar->mesh.numberOfVertices; v++)
    {
        fprintf(file,"v %.6f %.6f %.6f\n",positions[v*3+0],positions[v*3+1],positions[v*3+2]);
    }

    /* OBJ vertex references are one based. */
    for (unsigned int t=0; t<avatar->mesh.numberOfTriangles; t++)
    {
        fprintf(file,"f %u %u %u\n",
                avatar->mesh.indices[t*3+0]+1,
                avatar->mesh.indices[t*3+1]+1,
                avatar->mesh.indices[t*3+2]+1);
    }

    fclose(file);
    return ARF_OK;
}
