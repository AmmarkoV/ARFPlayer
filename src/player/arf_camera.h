/** @file arf_camera.h
 *  @brief A header-only orbit camera producing row-major matrices.
 *
 *  Row-major to match libarf's convention, so a model, view and projection can
 *  all be combined with arfMultiply4x4() and handed to glUniformMatrix4fv with
 *  transpose set to GL_TRUE.  Keeping one convention across the whole player is
 *  worth more than saving the transpose.
 *
 *  ARF containers hold camera-space data, in centimetres, with +Y up and the
 *  avatar at positive Z.  Which way it FACES is worth stating because it is
 *  not what the camera-space framing suggests: measured on the sample -- the
 *  vector from the head joint to the eye midpoint, averaged over the clip --
 *  the body faces +Z, the same direction it is displaced in.  So the front
 *  view is an eye on the +Z side looking back toward -Z, which is yaw = 0.
 *  An eye on the -Z side sees the avatar's back.
 *
 *  This is also why the player needs no coordinate flip anywhere: the pipeline
 *  renderer negates Y and Z because it draws with a fixed camera, whereas a
 *  lookAt placed on the correct side expresses the same view directly.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_CAMERA_H_INCLUDED
#define ARF_CAMERA_H_INCLUDED

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct arfCamera
{
    float target[3];      /**< the point the camera orbits, in centimetres */
    float distance;       /**< eye distance from the target */
    float yaw;            /**< radians, pi looks from -Z toward +Z */
    float pitch;          /**< radians, clamped just short of the poles */
    float fieldOfView;    /**< vertical, degrees */
    float nearPlane;
    float farPlane;
};

/** @brief Eye position implied by the current orbit parameters. */
static inline void arfCameraEye(const struct arfCamera *camera, float *eye)
{
    float cosPitch = cosf(camera->pitch);

    eye[0] = camera->target[0] + camera->distance * cosPitch * sinf(camera->yaw);
    eye[1] = camera->target[1] + camera->distance * sinf(camera->pitch);
    eye[2] = camera->target[2] + camera->distance * cosPitch * cosf(camera->yaw);
}

/** @brief Aim at a point and back off far enough for a sphere of `radius`
 *  around it to fill the vertical field of view. */
static inline void arfCameraFrame(struct arfCamera *camera, const float *centre, float radius)
{
    camera->target[0] = centre[0];
    camera->target[1] = centre[1];
    camera->target[2] = centre[2];

    if (radius < 1e-3f) { radius = 1e-3f; }

    camera->distance = 1.15f * radius / tanf(0.5f * camera->fieldOfView * (float) M_PI / 180.0f);

    /* Centimetre units with a person-sized subject: a near plane much below a
     * millimetre only spends depth precision, and the far plane has to clear
     * the whole clip even after the user zooms out. */
    camera->nearPlane = camera->distance * 0.01f;
    camera->farPlane  = camera->distance * 20.0f;
}

/** @brief Set the default view of a clip.
 *  @param centre where the motion sits, so the avatar never drifts out of frame
 *  @param radius bounding radius of a single pose, so one pose fills the view */
static inline void arfCameraReset(struct arfCamera *camera, const float *centre, float radius)
{
    camera->fieldOfView = 45.0f;
    camera->yaw   = 0.0f;           /* +Z side: the avatar faces the camera */
    camera->pitch = 0.0f;
    arfCameraFrame(camera,centre,radius);
}

static inline void arfCameraOrbit(struct arfCamera *camera, float deltaYaw, float deltaPitch)
{
    camera->yaw   += deltaYaw;
    camera->pitch += deltaPitch;

    /* Stopping short of straight up keeps the view's up vector from becoming
     * parallel to the forward vector, which would collapse the basis. */
    const float limit = 1.55f;
    if (camera->pitch >  limit) { camera->pitch =  limit; }
    if (camera->pitch < -limit) { camera->pitch = -limit; }
}

static inline void arfCameraZoom(struct arfCamera *camera, float factor)
{
    camera->distance *= factor;
    if (camera->distance < 1.0f) { camera->distance = 1.0f; }
}

/** @brief Slide the target across the view plane.  Deltas are in pixels and
 *  are scaled by distance so panning feels the same at any zoom. */
static inline void arfCameraPan(struct arfCamera *camera, float deltaX, float deltaY, int viewportHeight)
{
    float scale = 2.0f * camera->distance * tanf(0.5f * camera->fieldOfView * (float) M_PI / 180.0f) /
                  (float) ((viewportHeight > 0) ? viewportHeight : 1);

    float right[3] = { cosf(camera->yaw), 0.0f, -sinf(camera->yaw) };
    float up[3]    = { -sinf(camera->pitch) * sinf(camera->yaw),
                        cosf(camera->pitch),
                       -sinf(camera->pitch) * cosf(camera->yaw) };

    for (unsigned int c=0; c<3; c++)
    {
        camera->target[c] -= right[c] * deltaX * scale;
        camera->target[c] += up[c]    * deltaY * scale;
    }
}

/** @brief Row-major view matrix, equivalent to gluLookAt's transpose. */
static inline void arfCameraViewMatrix(const struct arfCamera *camera, float *view)
{
    float eye[3];
    arfCameraEye(camera,eye);

    float forward[3];
    float length = 0.0f;
    for (unsigned int c=0; c<3; c++)
    {
        forward[c] = camera->target[c] - eye[c];
        length += forward[c] * forward[c];
    }
    length = sqrtf(length);
    if (length < 1e-9f) { length = 1e-9f; }
    for (unsigned int c=0; c<3; c++) { forward[c] /= length; }

    const float worldUp[3] = { 0.0f, 1.0f, 0.0f };

    float right[3] =
    {
        forward[1]*worldUp[2] - forward[2]*worldUp[1],
        forward[2]*worldUp[0] - forward[0]*worldUp[2],
        forward[0]*worldUp[1] - forward[1]*worldUp[0]
    };

    length = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
    if (length < 1e-9f) { length = 1e-9f; }
    for (unsigned int c=0; c<3; c++) { right[c] /= length; }

    float up[3] =
    {
        right[1]*forward[2] - right[2]*forward[1],
        right[2]*forward[0] - right[0]*forward[2],
        right[0]*forward[1] - right[1]*forward[0]
    };

    view[0]  =  right[0];   view[1]  =  right[1];   view[2]  =  right[2];
    view[4]  =  up[0];      view[5]  =  up[1];      view[6]  =  up[2];
    view[8]  = -forward[0]; view[9]  = -forward[1]; view[10] = -forward[2];

    view[3]  = -(right[0]*eye[0]   + right[1]*eye[1]   + right[2]*eye[2]);
    view[7]  = -(up[0]*eye[0]      + up[1]*eye[1]      + up[2]*eye[2]);
    view[11] =  (forward[0]*eye[0] + forward[1]*eye[1] + forward[2]*eye[2]);

    view[12] = 0.0f; view[13] = 0.0f; view[14] = 0.0f; view[15] = 1.0f;
}

/** @brief Row-major perspective projection. */
static inline void arfCameraProjectionMatrix(const struct arfCamera *camera, float aspect, float *projection)
{
    float focal = 1.0f / tanf(0.5f * camera->fieldOfView * (float) M_PI / 180.0f);
    float depth = camera->nearPlane - camera->farPlane;

    for (unsigned int i=0; i<16; i++) { projection[i] = 0.0f; }

    projection[0]  = focal / ((aspect > 1e-6f) ? aspect : 1e-6f);
    projection[5]  = focal;
    projection[10] = (camera->farPlane + camera->nearPlane) / depth;
    projection[11] = (2.0f * camera->farPlane * camera->nearPlane) / depth;
    projection[14] = -1.0f;
}

#endif /* ARF_CAMERA_H_INCLUDED */
