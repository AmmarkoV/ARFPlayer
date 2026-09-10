/** @file arf_render.h
 *  @brief OpenGL 3.3 core rendering of a posed ARF avatar, plus the flat
 *         rectangles the timeline is drawn from.
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_RENDER_H_INCLUDED
#define ARF_RENDER_H_INCLUDED

#include "../libarf/arf.h"

struct arfRenderer;

/** @brief Compile the shaders and allocate the mesh buffers for one avatar.
 *  @param shaderDirectory where default.vert and default.frag live
 *  @retval a renderer to destroy with arfRendererDestroy(), or 0 */
struct arfRenderer *arfRendererCreate(const struct arfAvatar *avatar, const char *shaderDirectory);

/** @brief Release the renderer and its GL objects. */
void arfRendererDestroy(struct arfRenderer *renderer);

/** @brief Clear the framebuffer and set the viewport for a new frame. */
void arfRendererBeginFrame(int width, int height);

/** @brief Push a freshly evaluated pose's positions and normals to the GPU. */
void arfRendererUpload(struct arfRenderer *renderer, const struct arfPose *pose);

/** @brief Draw the uploaded mesh.
 *  @param modelViewProjection row-major, transposed on upload
 *  @param view                row-major, used to take normals into view space
 *  @param color               three components, 0 to 1 */
void arfRendererDrawMesh(struct arfRenderer *renderer, const float *modelViewProjection,
                         const float *view, const float *color);

/** @brief Draw a flat rectangle in screen coordinates, origin bottom left,
 *  both axes running 0 to 1.  Used for the timeline. */
void arfRendererDrawRect(struct arfRenderer *renderer, float x0, float y0, float x1, float y1,
                         float red, float green, float blue, float alpha);

#endif /* ARF_RENDER_H_INCLUDED */
