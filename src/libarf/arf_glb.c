/** @file arf_glb.c
 *  @brief The minimal GLB encoder/decoder declared in arf_glb.h.
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf_glb.h"
#include "arf_json.h"
#include "arf_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARF_GLB_MAGIC        0x46546C67u  /* "glTF" */
#define ARF_GLB_VERSION      2u
#define ARF_GLB_CHUNK_JSON   0x4E4F534Au  /* "JSON" */
#define ARF_GLB_CHUNK_BIN    0x004E4942u  /* "BIN\0" */

#define ARF_GLTF_FLOAT          5126
#define ARF_GLTF_UNSIGNED_INT   5125
#define ARF_GLTF_TARGET_ARRAY   34962  /* ARRAY_BUFFER, for POSITION */
#define ARF_GLTF_TARGET_INDEX   34963  /* ELEMENT_ARRAY_BUFFER, for indices */


/* ===========================================================================
 *  Tiny text helpers, local to this file -- same idiom arf_writer.c uses for
 *  arf.json, kept separate rather than shared to avoid coupling the two.
 * =========================================================================*/

static void arfGlbText(struct arfBuffer *buffer, const char *text)
{
    arfBufferAppend(buffer,text,strlen(text));
}

static void arfGlbUnsigned(struct arfBuffer *buffer, unsigned long long value)
{
    char text[32];
    snprintf(text,sizeof(text),"%llu",value);
    arfGlbText(buffer,text);
}

static void arfGlbFloat(struct arfBuffer *buffer, float value)
{
    char text[32];
    snprintf(text,sizeof(text),"%.9g",(double) value);
    arfGlbText(buffer,text);
}


/* ===========================================================================
 *  Writing
 * =========================================================================*/

void arfGlbWriteMesh(struct arfBuffer *out, const float *positions, unsigned int numberOfVertices,
                     const unsigned int *indices, unsigned int numberOfTriangles)
{
    size_t positionsBytes = (size_t) numberOfVertices * 3 * sizeof(float);
    size_t indicesBytes   = (size_t) numberOfTriangles * 3 * sizeof(unsigned int);

    float minimum[3] = {  1e30f,  1e30f,  1e30f };
    float maximum[3] = { -1e30f, -1e30f, -1e30f };
    for (unsigned int v=0; v<numberOfVertices; v++)
    {
        for (unsigned int c=0; c<3; c++)
        {
            float value = positions[v*3+c];
            if (value < minimum[c]) { minimum[c] = value; }
            if (value > maximum[c]) { maximum[c] = value; }
        }
    }

    /* -- JSON chunk -- */
    struct arfBuffer json;
    arfBufferInit(&json);

    arfGlbText(&json,"{\"asset\":{\"version\":\"2.0\"},");

    arfGlbText(&json,"\"buffers\":[{\"byteLength\":");
    arfGlbUnsigned(&json,(unsigned long long) (positionsBytes + indicesBytes));
    arfGlbText(&json,"}],");

    arfGlbText(&json,"\"bufferViews\":[");
    arfGlbText(&json,"{\"buffer\":0,\"byteOffset\":0,\"byteLength\":");
    arfGlbUnsigned(&json,(unsigned long long) positionsBytes);
    arfGlbText(&json,",\"target\":");
    arfGlbUnsigned(&json,ARF_GLTF_TARGET_ARRAY);
    arfGlbText(&json,"},");
    arfGlbText(&json,"{\"buffer\":0,\"byteOffset\":");
    arfGlbUnsigned(&json,(unsigned long long) positionsBytes);
    arfGlbText(&json,",\"byteLength\":");
    arfGlbUnsigned(&json,(unsigned long long) indicesBytes);
    arfGlbText(&json,",\"target\":");
    arfGlbUnsigned(&json,ARF_GLTF_TARGET_INDEX);
    arfGlbText(&json,"}],");

    arfGlbText(&json,"\"accessors\":[");
    arfGlbText(&json,"{\"bufferView\":0,\"componentType\":");
    arfGlbUnsigned(&json,ARF_GLTF_FLOAT);
    arfGlbText(&json,",\"count\":");
    arfGlbUnsigned(&json,numberOfVertices);
    arfGlbText(&json,",\"type\":\"VEC3\",\"min\":[");
    for (unsigned int c=0; c<3; c++) { if (c>0) { arfGlbText(&json,","); } arfGlbFloat(&json,minimum[c]); }
    arfGlbText(&json,"],\"max\":[");
    for (unsigned int c=0; c<3; c++) { if (c>0) { arfGlbText(&json,","); } arfGlbFloat(&json,maximum[c]); }
    arfGlbText(&json,"]},");
    arfGlbText(&json,"{\"bufferView\":1,\"componentType\":");
    arfGlbUnsigned(&json,ARF_GLTF_UNSIGNED_INT);
    arfGlbText(&json,",\"count\":");
    arfGlbUnsigned(&json,(unsigned long long) numberOfTriangles * 3);
    arfGlbText(&json,",\"type\":\"SCALAR\"}],");

    arfGlbText(&json,"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":4}]}]}");

    /* Pad the JSON chunk to a 4-byte boundary with spaces, per the GLB spec. */
    while (json.length % 4 != 0) { arfBufferWriteU8(&json,' '); }

    /* -- BIN chunk: positions then indices, contiguous -- both already
     * 4-byte-aligned since positionsBytes is a multiple of 4. -- */
    struct arfBuffer bin;
    arfBufferInit(&bin);
    arfBufferWriteFloats(&bin,positions,(size_t) numberOfVertices * 3);
    arfBufferWriteU32s(&bin,indices,(size_t) numberOfTriangles * 3);
    /* No padding needed: both source arrays are already multiples of 4 bytes. */

    /* -- Assemble the container: 12 byte header, JSON chunk, BIN chunk -- */
    unsigned int totalLength = (unsigned int) (12 + 8 + json.length + 8 + bin.length);

    arfBufferWriteU32(out,ARF_GLB_MAGIC);
    arfBufferWriteU32(out,ARF_GLB_VERSION);
    arfBufferWriteU32(out,totalLength);

    arfBufferWriteU32(out,(unsigned int) json.length);
    arfBufferWriteU32(out,ARF_GLB_CHUNK_JSON);
    arfBufferAppend(out,json.data,json.length);

    arfBufferWriteU32(out,(unsigned int) bin.length);
    arfBufferWriteU32(out,ARF_GLB_CHUNK_BIN);
    arfBufferAppend(out,bin.data,bin.length);

    arfBufferFree(&json);
    arfBufferFree(&bin);
}


/* ===========================================================================
 *  Reading
 * =========================================================================*/

/** @brief Look up components.schemas-style int fields the way this file's
 *  own writer emits them: object member, plain number, no fallback. */
static int arfGlbRequireInt(const struct arfJsonValue *object, const char *key, const char *what, int *out)
{
    const struct arfJsonValue *member = arfJsonMember(object,key);
    if (member==0) { arfSetError("%s: GLB JSON is missing \"%s\"",what,key); return 0; }

    *out = (int) arfJsonNumber(member,-1.0);
    return 1;
}

int arfGlbReadMesh(const void *bytes, size_t size, const char *what,
                   float **outPositions, unsigned int *outNumberOfVertices,
                   unsigned int **outIndices, unsigned int *outNumberOfTriangles)
{
    *outPositions = 0;
    *outIndices   = 0;

    struct arfCursor cursor;
    arfCursorInit(&cursor,bytes,size);

    unsigned int magic   = arfCursorReadU32(&cursor);
    unsigned int version = arfCursorReadU32(&cursor);
    unsigned int total   = arfCursorReadU32(&cursor);

    if (cursor.failed || (magic!=ARF_GLB_MAGIC))
    {
        arfSetError("%s: not a GLB container (bad magic)",what);
        return 0;
    }
    if (version != ARF_GLB_VERSION)
    {
        arfSetError("%s: GLB version %u, expected %u",what,version,ARF_GLB_VERSION);
        return 0;
    }
    if (total != (unsigned int) size)
    {
        arfSetError("%s: GLB declares total length %u but the entry is %zu bytes",what,total,size);
        return 0;
    }

    unsigned int jsonLength = arfCursorReadU32(&cursor);
    unsigned int jsonType   = arfCursorReadU32(&cursor);
    const unsigned char *jsonBytes = arfCursorTake(&cursor,jsonLength);

    if ( (jsonBytes==0) || (jsonType!=ARF_GLB_CHUNK_JSON) )
    {
        arfSetError("%s: GLB's first chunk is not a JSON chunk",what);
        return 0;
    }

    unsigned int binLength = arfCursorReadU32(&cursor);
    unsigned int binType   = arfCursorReadU32(&cursor);
    const unsigned char *binBytes = arfCursorTake(&cursor,binLength);

    if ( (binBytes==0) || (binType!=ARF_GLB_CHUNK_BIN) )
    {
        arfSetError("%s: GLB has no BIN chunk",what);
        return 0;
    }

    const char *jsonError = 0;
    struct arfJsonValue *document = arfJsonParse((const char *) jsonBytes,jsonLength,&jsonError);
    if (document==0) { arfSetError("%s: GLB JSON chunk is not valid JSON: %s",what,jsonError); return 0; }

    int result = 0;

    const struct arfJsonValue *mesh      = arfJsonAt(arfJsonMember(document,"meshes"),0);
    const struct arfJsonValue *primitive = arfJsonAt(arfJsonMember(mesh,"primitives"),0);
    if (primitive==0) { arfSetError("%s: GLB has no meshes[0].primitives[0]",what); goto done; }

    const struct arfJsonValue *attributes = arfJsonMember(primitive,"attributes");
    const struct arfJsonValue *positionRef = arfJsonMember(attributes,"POSITION");
    const struct arfJsonValue *indicesRef  = arfJsonMember(primitive,"indices");
    if ( (positionRef==0) || (indicesRef==0) )
    {
        arfSetError("%s: GLB primitive is missing POSITION or indices",what);
        goto done;
    }

    const struct arfJsonValue *accessors   = arfJsonMember(document,"accessors");
    const struct arfJsonValue *bufferViews = arfJsonMember(document,"bufferViews");

    const struct arfJsonValue *positionAccessor = arfJsonAt(accessors,(unsigned int) arfJsonNumber(positionRef,-1.0));
    const struct arfJsonValue *indexAccessor    = arfJsonAt(accessors,(unsigned int) arfJsonNumber(indicesRef,-1.0));
    if ( (positionAccessor==0) || (indexAccessor==0) )
    {
        arfSetError("%s: GLB accessor reference is out of range",what);
        goto done;
    }

    int positionComponentType=0, positionBufferViewIndex=0, positionCount=0;
    int indexComponentType=0,    indexBufferViewIndex=0,    indexCount=0;

    if ( !arfGlbRequireInt(positionAccessor,"componentType",what,&positionComponentType) ||
         !arfGlbRequireInt(positionAccessor,"bufferView",what,&positionBufferViewIndex) ||
         !arfGlbRequireInt(positionAccessor,"count",what,&positionCount) ||
         !arfGlbRequireInt(indexAccessor,"componentType",what,&indexComponentType) ||
         !arfGlbRequireInt(indexAccessor,"bufferView",what,&indexBufferViewIndex) ||
         !arfGlbRequireInt(indexAccessor,"count",what,&indexCount) )
    {
        goto done;
    }

    const char *positionType = arfJsonString(arfJsonMember(positionAccessor,"type"),"");
    const char *indexType    = arfJsonString(arfJsonMember(indexAccessor,"type"),"");

    if ( (positionComponentType!=ARF_GLTF_FLOAT) || (strcmp(positionType,"VEC3")!=0) )
    {
        arfSetError("%s: GLB POSITION accessor must be FLOAT VEC3",what);
        goto done;
    }
    if ( (indexComponentType!=ARF_GLTF_UNSIGNED_INT) || (strcmp(indexType,"SCALAR")!=0) )
    {
        arfSetError("%s: GLB indices accessor must be UNSIGNED_INT SCALAR "
                    "(this reader only reads what arfGlbWriteMesh() itself writes)",what);
        goto done;
    }
    if (indexCount % 3 != 0)
    {
        arfSetError("%s: GLB indices count %d is not a multiple of 3",what,indexCount);
        goto done;
    }

    const struct arfJsonValue *positionView = arfJsonAt(bufferViews,(unsigned int) positionBufferViewIndex);
    const struct arfJsonValue *indexView    = arfJsonAt(bufferViews,(unsigned int) indexBufferViewIndex);
    if ( (positionView==0) || (indexView==0) )
    {
        arfSetError("%s: GLB bufferView reference is out of range",what);
        goto done;
    }

    int positionBuffer=0, positionOffset=0, positionByteLength=0;
    int indexBuffer=0,    indexOffset=0,    indexByteLength=0;

    if ( !arfGlbRequireInt(positionView,"buffer",what,&positionBuffer) ||
         !arfGlbRequireInt(positionView,"byteOffset",what,&positionOffset) ||
         !arfGlbRequireInt(positionView,"byteLength",what,&positionByteLength) ||
         !arfGlbRequireInt(indexView,"buffer",what,&indexBuffer) ||
         !arfGlbRequireInt(indexView,"byteOffset",what,&indexOffset) ||
         !arfGlbRequireInt(indexView,"byteLength",what,&indexByteLength) )
    {
        goto done;
    }

    if ( (positionBuffer!=0) || (indexBuffer!=0) )
    {
        arfSetError("%s: GLB references more than one buffer, not supported here",what);
        goto done;
    }

    size_t positionNeed = (size_t) positionCount * 3 * sizeof(float);
    size_t indexNeed     = (size_t) indexCount * sizeof(unsigned int);

    if ( (positionOffset<0) || (indexOffset<0) ||
         ((size_t) positionByteLength < positionNeed) || ((size_t) indexByteLength < indexNeed) ||
         ((size_t) positionOffset + positionNeed > binLength) ||
         ((size_t) indexOffset + indexNeed > binLength) )
    {
        arfSetError("%s: GLB BIN chunk is too small for its own accessors",what);
        goto done;
    }

    float        *positions = (float *)        malloc(positionNeed);
    unsigned int *indices   = (unsigned int *) malloc(indexNeed);
    if ( (positions==0) || (indices==0) )
    {
        arfSetError("%s: out of memory decoding GLB mesh",what);
        free(positions);
        free(indices);
        goto done;
    }

    struct arfCursor positionCursor;
    arfCursorInit(&positionCursor,binBytes + positionOffset,positionNeed);
    struct arfCursor indexCursor;
    arfCursorInit(&indexCursor,binBytes + indexOffset,indexNeed);

    if ( !arfCursorReadFloats(&positionCursor,positions,(size_t) positionCount * 3) ||
         !arfCursorReadU32s(&indexCursor,indices,(size_t) indexCount) )
    {
        arfSetError("%s: GLB BIN chunk is truncated",what);
        free(positions);
        free(indices);
        goto done;
    }

    *outPositions         = positions;
    *outNumberOfVertices  = (unsigned int) positionCount;
    *outIndices           = indices;
    *outNumberOfTriangles = (unsigned int) (indexCount / 3);
    result = 1;

done:
    arfJsonFree(document);
    return result;
}
