/** @file arf_skin.c
 *  @brief Hierarchy composition, blendshapes, linear blend skinning and normal
 *         recomputation -- the whole per-frame job of an ARF player.
 *
 *  Every matrix here is row-major with the translation in the last column, the
 *  convention the container itself uses.  Nothing in this file touches OpenGL;
 *  a viewer transposes on upload.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf.h"
#include "arf_internal.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

void arfIdentity4x4(float *m)
{
    memset(m,0,16*sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

void arfMultiply4x4(float *result, const float *a, const float *b)
{
    float scratch[16];

    for (unsigned int row=0; row<4; row++)
    {
        for (unsigned int column=0; column<4; column++)
        {
            scratch[row*4+column] = a[row*4+0] * b[0*4+column] +
                                    a[row*4+1] * b[1*4+column] +
                                    a[row*4+2] * b[2*4+column] +
                                    a[row*4+3] * b[3*4+column];
        }
    }

    memcpy(result,scratch,16*sizeof(float));
}

void arfTranspose4x4(float *result, const float *m)
{
    float scratch[16];

    for (unsigned int row=0; row<4; row++)
    {
        for (unsigned int column=0; column<4; column++)
        {
            scratch[row*4+column] = m[column*4+row];
        }
    }

    memcpy(result,scratch,16*sizeof(float));
}

void arfComposeGlobals(const struct arfAvatar *avatar, const float *localMatrices, float *globals)
{
    if ( (avatar==0) || (localMatrices==0) || (globals==0) ) { return; }

    /* One forward pass is enough because the reader guarantees every parent is
     * listed before its children. */
    for (unsigned int j=0; j<avatar->numberOfNodes; j++)
    {
        const float *local  = localMatrices + (size_t) j * 16;
        float       *global = globals       + (size_t) j * 16;
        int          parent = avatar->nodes[j].parent;

        if (parent < 0) { memcpy(global,local,16*sizeof(float));                        }
        else            { arfMultiply4x4(global,globals + (size_t) parent * 16,local);  }
    }
}

void arfComputeSkinMatrices(const struct arfAvatar *avatar, const float *globals, float *skinMatrices)
{
    if ( (avatar==0) || (globals==0) || (skinMatrices==0) )      { return; }
    if (avatar->skin.inverseBindMatrices==0)                     { return; }

    for (unsigned int j=0; j<avatar->numberOfNodes; j++)
    {
        arfMultiply4x4(skinMatrices + (size_t) j * 16,
                       globals + (size_t) j * 16,
                       avatar->skin.inverseBindMatrices + (size_t) j * 16);
    }
}

void arfApplyBlendshapes(const struct arfAvatar *avatar, const float *weights, float *positions)
{
    if ( (avatar==0) || (positions==0) ) { return; }

    size_t componentCount = (size_t) avatar->mesh.numberOfVertices * 3;
    memcpy(positions,avatar->mesh.positions,componentCount * sizeof(float));

    if ( (weights==0) || (avatar->blendshapes.deltas==0) ) { return; }

    for (unsigned int s=0; s<avatar->blendshapes.numberOfShapes; s++)
    {
        float weight = weights[s];

        /* Facial expression weights are mostly zero on any given frame, so the
         * skip is worth more than it looks: it turns a 72-target pass over the
         * whole mesh into a handful of them. */
        if ( (weight > -1e-6f) && (weight < 1e-6f) ) { continue; }

        const float *deltas = avatar->blendshapes.deltas + (size_t) s * componentCount;
        for (size_t i=0; i<componentCount; i++) { positions[i] += weight * deltas[i]; }
    }
}

void arfSkinVertices(const struct arfAvatar *avatar, const float *skinMatrices,
                     const float *restPositions, float *positions)
{
    if ( (avatar==0) || (skinMatrices==0) || (restPositions==0) || (positions==0) ) { return; }
    if (avatar->skin.vertexStart==0)                                                { return; }

    for (unsigned int v=0; v<avatar->mesh.numberOfVertices; v++)
    {
        const float *rest = restPositions + (size_t) v * 3;
        float        x = 0.0f, y = 0.0f, z = 0.0f;

        for (unsigned int e=avatar->skin.vertexStart[v]; e<avatar->skin.vertexStart[v+1]; e++)
        {
            const float *m = skinMatrices + (size_t) avatar->skin.jointIndex[e] * 16;
            float        w = avatar->skin.weight[e];

            x += w * (m[0]*rest[0] + m[1]*rest[1] + m[2]*rest[2]  + m[3]);
            y += w * (m[4]*rest[0] + m[5]*rest[1] + m[6]*rest[2]  + m[7]);
            z += w * (m[8]*rest[0] + m[9]*rest[1] + m[10]*rest[2] + m[11]);
        }

        positions[v*3+0] = x;
        positions[v*3+1] = y;
        positions[v*3+2] = z;
    }
}

void arfRecomputeNormals(const struct arfAvatar *avatar, const float *positions, float *normals)
{
    if ( (avatar==0) || (positions==0) || (normals==0) ) { return; }

    unsigned int vertexCount   = avatar->mesh.numberOfVertices;
    unsigned int triangleCount = avatar->mesh.numberOfTriangles;
    const unsigned int *index  = avatar->mesh.indices;

    memset(normals,0,(size_t) vertexCount * 3 * sizeof(float));

    for (unsigned int t=0; t<triangleCount; t++)
    {
        unsigned int i0 = index[t*3+0], i1 = index[t*3+1], i2 = index[t*3+2];
        const float *p0 = positions + (size_t) i0 * 3;
        const float *p1 = positions + (size_t) i1 * 3;
        const float *p2 = positions + (size_t) i2 * 3;

        float e1x = p1[0]-p0[0], e1y = p1[1]-p0[1], e1z = p1[2]-p0[2];
        float e2x = p2[0]-p0[0], e2y = p2[1]-p0[1], e2z = p2[2]-p0[2];

        /* Left unnormalised on purpose: the cross product's magnitude is twice
         * the triangle area, so accumulating it gives the standard
         * area-weighted vertex normal for free. */
        float fx = e1y*e2z - e1z*e2y;
        float fy = e1z*e2x - e1x*e2z;
        float fz = e1x*e2y - e1y*e2x;

        normals[i0*3+0] += fx; normals[i0*3+1] += fy; normals[i0*3+2] += fz;
        normals[i1*3+0] += fx; normals[i1*3+1] += fy; normals[i1*3+2] += fz;
        normals[i2*3+0] += fx; normals[i2*3+1] += fy; normals[i2*3+2] += fz;
    }

    for (unsigned int v=0; v<vertexCount; v++)
    {
        float x = normals[v*3+0], y = normals[v*3+1], z = normals[v*3+2];
        float length = sqrtf(x*x + y*y + z*z);

        if (length > 1e-12f)
        {
            normals[v*3+0] = x / length;
            normals[v*3+1] = y / length;
            normals[v*3+2] = z / length;
        }
    }
}

int arfRebuildSkinIndex(struct arfAvatar *avatar)
{
    if (avatar==0) { arfSetError("no avatar was given"); return ARF_ERROR_ARGUMENT; }

    unsigned int vertexCount = avatar->mesh.numberOfVertices;
    unsigned int weightCount = avatar->skin.numberOfWeights;

    free(avatar->skin.vertexStart);
    avatar->skin.vertexStart = (unsigned int *) calloc((size_t) vertexCount + 1,sizeof(unsigned int));
    if (avatar->skin.vertexStart==0) { arfSetError("out of memory building the skin index"); return ARF_ERROR_MEMORY; }

    /* Counting sort of the COO entries into per-vertex runs.  The container's
     * entries already arrive vertex-major, but nothing in the format promises
     * that, and a player that assumed it would skin a scrambled mesh in
     * silence. */
    for (unsigned int e=0; e<weightCount; e++)
    {
        unsigned int v = avatar->skin.vertexIndex[e];
        if (v >= vertexCount) { arfSetError("skin weight %u addresses vertex %u of %u",e,v,vertexCount); return ARF_ERROR_FORMAT; }
        avatar->skin.vertexStart[v+1]++;
    }

    for (unsigned int v=0; v<vertexCount; v++)
    {
        avatar->skin.vertexStart[v+1] += avatar->skin.vertexStart[v];
    }

    unsigned int *sortedJoint  = (unsigned int *) malloc((size_t) weightCount * sizeof(unsigned int) + 1);
    float        *sortedWeight = (float *)        malloc((size_t) weightCount * sizeof(float) + 1);
    unsigned int *fill         = (unsigned int *) malloc((size_t) vertexCount * sizeof(unsigned int) + 1);

    if ( (sortedJoint==0) || (sortedWeight==0) || (fill==0) )
    {
        free(sortedJoint);
        free(sortedWeight);
        free(fill);
        arfSetError("out of memory sorting %u skin weights",weightCount);
        return ARF_ERROR_MEMORY;
    }

    memcpy(fill,avatar->skin.vertexStart,(size_t) vertexCount * sizeof(unsigned int));

    for (unsigned int e=0; e<weightCount; e++)
    {
        unsigned int slot = fill[avatar->skin.vertexIndex[e]]++;
        sortedJoint[slot]  = avatar->skin.jointIndex[e];
        sortedWeight[slot] = avatar->skin.weight[e];
    }

    for (unsigned int e=0; e<weightCount; e++)
    {
        avatar->skin.jointIndex[e] = sortedJoint[e];
        avatar->skin.weight[e]     = sortedWeight[e];
    }

    /* vertexIndex is rewritten to match the new ordering so that all three
     * parallel arrays stay consistent for anyone reading them directly. */
    for (unsigned int v=0; v<vertexCount; v++)
    {
        for (unsigned int e=avatar->skin.vertexStart[v]; e<avatar->skin.vertexStart[v+1]; e++)
        {
            avatar->skin.vertexIndex[e] = v;
        }
    }

    free(sortedJoint);
    free(sortedWeight);
    free(fill);
    return ARF_OK;
}

struct arfPose *arfPoseAllocate(struct arfAvatar *avatar)
{
    if (avatar==0) { arfSetError("no avatar was given"); return 0; }

    if (avatar->skin.vertexStart==0)
    {
        if (arfRebuildSkinIndex(avatar)!=ARF_OK) { return 0; }
    }

    struct arfPose *pose = (struct arfPose *) calloc(1,sizeof(struct arfPose));
    if (pose==0) { arfSetError("out of memory allocating a pose"); return 0; }

    pose->numberOfJoints   = avatar->numberOfNodes;
    pose->numberOfVertices = avatar->mesh.numberOfVertices;

    size_t matrixFloats = (size_t) pose->numberOfJoints * 16;
    size_t vertexFloats = (size_t) pose->numberOfVertices * 3;

    pose->jointGlobals  = (float *) malloc(matrixFloats * sizeof(float));
    pose->skinMatrices  = (float *) malloc(matrixFloats * sizeof(float));
    pose->restPositions = (float *) malloc(vertexFloats * sizeof(float));
    pose->positions     = (float *) malloc(vertexFloats * sizeof(float));
    pose->normals       = (float *) malloc(vertexFloats * sizeof(float));

    if ( (pose->jointGlobals==0) || (pose->skinMatrices==0) || (pose->restPositions==0) ||
         (pose->positions==0)    || (pose->normals==0) )
    {
        arfPoseFree(pose);
        arfSetError("out of memory allocating pose buffers");
        return 0;
    }

    return pose;
}

void arfPoseFree(struct arfPose *pose)
{
    if (pose==0) { return; }

    free(pose->jointGlobals);
    free(pose->skinMatrices);
    free(pose->restPositions);
    free(pose->positions);
    free(pose->normals);
    free(pose);
}

int arfPoseEvaluate(const struct arfAvatar *avatar, struct arfPose *pose, unsigned int frame)
{
    if ( (avatar==0) || (pose==0) )        { arfSetError("no avatar or pose was given"); return ARF_ERROR_ARGUMENT; }
    if (frame >= avatar->numberOfFrames)   { arfSetError("frame %u is past the end of a %u frame clip",frame,avatar->numberOfFrames); return ARF_ERROR_ARGUMENT; }

    const float *weights = 0;
    if ( (avatar->hasFace) && (avatar->numberOfFaceFrames > 0) )
    {
        /* The two tracks are written frame for frame, but a container is free
         * to carry a shorter face track, so the last expression is held. */
        unsigned int faceFrame = (frame < avatar->numberOfFaceFrames) ? frame : avatar->numberOfFaceFrames - 1;
        weights = avatar->blendshapeWeights + (size_t) faceFrame * avatar->blendshapes.numberOfShapes;
    }

    const float *localMatrices = avatar->localMatrices + (size_t) frame * avatar->numberOfNodes * 16;

    arfApplyBlendshapes(avatar,weights,pose->restPositions);
    arfComposeGlobals(avatar,localMatrices,pose->jointGlobals);
    arfComputeSkinMatrices(avatar,pose->jointGlobals,pose->skinMatrices);
    arfSkinVertices(avatar,pose->skinMatrices,pose->restPositions,pose->positions);
    arfRecomputeNormals(avatar,pose->positions,pose->normals);

    return ARF_OK;
}
