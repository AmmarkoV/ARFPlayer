/** @file arf_glb.h
 *  @brief A minimal binary glTF (GLB) encoder/decoder: one mesh, one
 *         primitive, a POSITION accessor and an indices accessor, no
 *         materials, no textures, no normals.  Nothing else.
 *
 *  This exists for exactly one reason: BlendshapeSet.shapes entries are, per
 *  spec, "GLB files that only have geometry information (vertices and
 *  faces)" -- one per blendshape target, each carrying the *absolute*
 *  deformed vertex positions (not a delta; the blend formula in the spec is
 *  v_out = v_0 + sum(w_i * (v_i - v_0)), i.e. v_i is the shape's own
 *  position, not v_i - v_0). This is not a general-purpose glTF library: it
 *  reads only what arfGlbWriteMesh() itself writes, and rejects anything
 *  else with a clear error rather than guessing.
 *
 *  GLB is always little-endian, unlike the AAU animation stream -- see the
 *  note in arf_format.h. The embedded JSON chunk is parsed with this
 *  library's own arf_json.c, so there is no second JSON parser here.
 *
 *  Not part of the public API.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_GLB_H_INCLUDED
#define ARF_GLB_H_INCLUDED

#include <stddef.h>
#include "arf_bytes.h"

/** @brief Encode one mesh as a minimal GLB: a POSITION accessor (float32
 *  VEC3) and an indices accessor (uint32 SCALAR), packed into a single
 *  embedded BIN chunk, no materials or textures.
 *  @param positions numberOfVertices * 3
 *  @param indices   numberOfTriangles * 3 */
void arfGlbWriteMesh(struct arfBuffer *out, const float *positions, unsigned int numberOfVertices,
                     const unsigned int *indices, unsigned int numberOfTriangles);

/** @brief Decode a GLB written by arfGlbWriteMesh(). Rejects anything with a
 *  different shape (multiple buffers, unexpected component types, a missing
 *  BIN chunk, ...) rather than guessing at it.
 *  @param what        a short label for error messages, e.g. "blendshape 3"
 *  @param outPositions receives a malloc'd numberOfVertices*3 array, caller frees
 *  @param outIndices   receives a malloc'd numberOfTriangles*3 array, caller frees
 *  @retval 1 on success, 0 on failure (arfSetError is set) */
int arfGlbReadMesh(const void *bytes, size_t size, const char *what,
                   float **outPositions, unsigned int *outNumberOfVertices,
                   unsigned int **outIndices, unsigned int *outNumberOfTriangles);

#endif /* ARF_GLB_H_INCLUDED */
