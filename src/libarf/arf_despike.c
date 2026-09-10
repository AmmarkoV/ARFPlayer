/** @file arf_despike.c
 *  @brief Repair of tracking-singularity glitch frames in an animation track.
 *
 *  The pose estimator behind these containers regresses each joint's local
 *  rotation as Euler angles from a linear PCA decode, then converts to a
 *  quaternion.  When a joint's rotation sits near the Euler representation's
 *  singularity -- which the pelvis does for the whole of a typical clip, its
 *  local rotation being a near-180 degree turn away from its rest prerotation
 *  -- a small change in the decoded angles can jump branches.  The result is a
 *  burst of two or three frames whose rotation is a perfectly valid matrix
 *  about 100 degrees away from the smooth trajectory either side of it.  One
 *  joint, a couple of frames, and the whole body folds up.
 *
 *  These are not readable as data errors: the matrices are orthogonal, unit
 *  determinant, correctly framed.  Only their velocity gives them away.  So
 *  the repair is the standard one -- flag frames whose joint angular velocity
 *  is implausible, then replace the flagged span by interpolating across it
 *  from the nearest clean frames either side.
 *
 *  This mirrors the two-stage despike in the producing project's GMR
 *  retargeting path (tools/gmr_retarget.py, despike_frames), which exists for
 *  the same glitches and defaults to the same 40 degree per frame threshold.
 *  That filter lives downstream of the container, so a .arfz still carries the
 *  raw frames and every reader meets them.
 *
 *  Nothing calls this automatically.  A reader that silently rewrote the
 *  animation it was asked to read would be worse than one that shows a glitch.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf.h"
#include "arf_internal.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/** @brief Split a row-major TRS matrix into translation, unit quaternion and
 *  uniform scale.  The container composes T * R * S with a single scale per
 *  joint, so the scale is recoverable as the rotation block's norm. */
static void arfDecomposeTRS(const float *m, float *translation, float *quaternion, float *scale)
{
    translation[0] = m[3];
    translation[1] = m[7];
    translation[2] = m[11];

    float sum = 0.0f;
    for (unsigned int row=0; row<3; row++)
    {
        for (unsigned int column=0; column<3; column++) { sum += m[row*4+column] * m[row*4+column]; }
    }

    float s = sqrtf(sum / 3.0f);
    if (s < 1e-12f) { s = 1e-12f; }
    *scale = s;

    float r[9];
    for (unsigned int row=0; row<3; row++)
    {
        for (unsigned int column=0; column<3; column++) { r[row*3+column] = m[row*4+column] / s; }
    }

    /* Shepperd's method: pick the largest of the four to divide by, so the
     * conversion stays conditioned at every orientation. */
    float trace = r[0] + r[4] + r[8];
    float x, y, z, w;

    if (trace > 0.0f)
    {
        float k = sqrtf(trace + 1.0f) * 2.0f;
        w = 0.25f * k;
        x = (r[7] - r[5]) / k;
        y = (r[2] - r[6]) / k;
        z = (r[3] - r[1]) / k;
    }
    else if ( (r[0] > r[4]) && (r[0] > r[8]) )
    {
        float k = sqrtf(1.0f + r[0] - r[4] - r[8]) * 2.0f;
        w = (r[7] - r[5]) / k;
        x = 0.25f * k;
        y = (r[1] + r[3]) / k;
        z = (r[2] + r[6]) / k;
    }
    else if (r[4] > r[8])
    {
        float k = sqrtf(1.0f + r[4] - r[0] - r[8]) * 2.0f;
        w = (r[2] - r[6]) / k;
        x = (r[1] + r[3]) / k;
        y = 0.25f * k;
        z = (r[5] + r[7]) / k;
    }
    else
    {
        float k = sqrtf(1.0f + r[8] - r[0] - r[4]) * 2.0f;
        w = (r[3] - r[1]) / k;
        x = (r[2] + r[6]) / k;
        y = (r[5] + r[7]) / k;
        z = 0.25f * k;
    }

    float length = sqrtf(x*x + y*y + z*z + w*w);
    if (length < 1e-12f) { length = 1.0f; }

    quaternion[0] = x / length;
    quaternion[1] = y / length;
    quaternion[2] = z / length;
    quaternion[3] = w / length;
}

/** @brief Rebuild a row-major T * R * S matrix from its parts.  XYZW. */
static void arfComposeTRS(float *m, const float *translation, const float *quaternion, float scale)
{
    float x = quaternion[0], y = quaternion[1], z = quaternion[2], w = quaternion[3];

    m[0]  = (1.0f - 2.0f*(y*y + z*z)) * scale;
    m[1]  = (2.0f*(x*y - w*z)) * scale;
    m[2]  = (2.0f*(x*z + w*y)) * scale;
    m[3]  = translation[0];

    m[4]  = (2.0f*(x*y + w*z)) * scale;
    m[5]  = (1.0f - 2.0f*(x*x + z*z)) * scale;
    m[6]  = (2.0f*(y*z - w*x)) * scale;
    m[7]  = translation[1];

    m[8]  = (2.0f*(x*z - w*y)) * scale;
    m[9]  = (2.0f*(y*z + w*x)) * scale;
    m[10] = (1.0f - 2.0f*(x*x + y*y)) * scale;
    m[11] = translation[2];

    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
}

/** @brief Shortest-arc angle between two unit quaternions, in degrees. */
static float arfQuaternionAngle(const float *a, const float *b)
{
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    if (dot < 0.0f) { dot = -dot; }        /* q and -q are the same rotation */
    if (dot > 1.0f) { dot = 1.0f; }

    return 2.0f * acosf(dot) * 57.2957795f;
}

static void arfQuaternionSlerp(float *out, const float *a, const float *b, float alpha)
{
    float from[4];
    memcpy(from,a,4*sizeof(float));

    float dot = from[0]*b[0] + from[1]*b[1] + from[2]*b[2] + from[3]*b[3];
    if (dot < 0.0f)
    {
        /* Take the short way round the double cover. */
        for (unsigned int i=0; i<4; i++) { from[i] = -from[i]; }
        dot = -dot;
    }
    if (dot > 1.0f) { dot = 1.0f; }

    float scaleFrom, scaleTo;
    if (dot > 0.9995f)
    {
        /* Nearly parallel -- lerp and renormalise, slerp would divide by ~0. */
        scaleFrom = 1.0f - alpha;
        scaleTo   = alpha;
    }
    else
    {
        float theta = acosf(dot);
        float sine  = sinf(theta);
        scaleFrom = sinf((1.0f - alpha) * theta) / sine;
        scaleTo   = sinf(alpha * theta) / sine;
    }

    float length = 0.0f;
    for (unsigned int i=0; i<4; i++)
    {
        out[i] = scaleFrom * from[i] + scaleTo * b[i];
        length += out[i] * out[i];
    }

    length = sqrtf(length);
    if (length < 1e-12f) { length = 1.0f; }
    for (unsigned int i=0; i<4; i++) { out[i] /= length; }
}

/** @brief Write one interpolated frame: read the two clean anchors from
 *  `source`, write the result into `destination`.  Source and destination are
 *  separate buffers so a repaired frame is never used as an anchor for the
 *  next one. */
static void arfInterpolateFrame(const float *source, float *destination, unsigned int jointCount,
                                unsigned int target, unsigned int before, unsigned int after, float alpha)
{
    size_t stride = (size_t) jointCount * 16;

    const float *low  = source + (size_t) before * stride;
    const float *high = source + (size_t) after  * stride;
    float       *out  = destination + (size_t) target * stride;

    for (unsigned int j=0; j<jointCount; j++)
    {
        float lowTranslation[3],  lowQuaternion[4],  lowScale;
        float highTranslation[3], highQuaternion[4], highScale;

        arfDecomposeTRS(low  + (size_t) j*16,lowTranslation, lowQuaternion, &lowScale);
        arfDecomposeTRS(high + (size_t) j*16,highTranslation,highQuaternion,&highScale);

        float translation[3];
        for (unsigned int c=0; c<3; c++)
        {
            translation[c] = lowTranslation[c] + alpha * (highTranslation[c] - lowTranslation[c]);
        }

        float quaternion[4];
        arfQuaternionSlerp(quaternion,lowQuaternion,highQuaternion,alpha);

        arfComposeTRS(out + (size_t) j*16,translation,quaternion,
                      lowScale + alpha * (highScale - lowScale));
    }
}

/** @brief Largest per-joint angle between two frames' quaternions, in degrees. */
static float arfFrameDistance(const float *a, const float *b, unsigned int jointCount)
{
    float worst = 0.0f;
    for (unsigned int j=0; j<jointCount; j++)
    {
        float angle = arfQuaternionAngle(a + (size_t) j*4,b + (size_t) j*4);
        if (angle > worst) { worst = angle; }
    }
    return worst;
}

int arfDespikeFrames(struct arfAvatar *avatar, float maxDegreesPerFrame, unsigned int *repairedFrames)
{
    if (repairedFrames!=0) { *repairedFrames = 0; }

    if (avatar==0)                    { arfSetError("no avatar was given"); return ARF_ERROR_ARGUMENT; }
    if (!(maxDegreesPerFrame > 0.0f)) { arfSetError("the despike threshold must be positive"); return ARF_ERROR_ARGUMENT; }
    if (avatar->numberOfFrames < 3)   { return ARF_OK; }

    unsigned int frameCount = avatar->numberOfFrames;
    unsigned int jointCount = avatar->numberOfNodes;
    size_t       stride     = (size_t) jointCount * 16;

    int    result     = ARF_OK;
    float *quaternion = (float *) malloc((size_t) frameCount * jointCount * 4 * sizeof(float));
    char  *bad        = (char *)  calloc(frameCount,1);
    float *original   = 0;
    float *candidate  = (float *) malloc((size_t) jointCount * 4 * sizeof(float));

    if ( (quaternion==0) || (bad==0) || (candidate==0) )
    {
        arfSetError("out of memory despiking %u frames",frameCount);
        result = ARF_ERROR_MEMORY;
        goto done;
    }

    /* Decompose once -- every stage below works on rotations alone. */
    for (unsigned int f=0; f<frameCount; f++)
    {
        const float *matrices = avatar->localMatrices + (size_t) f * stride;
        float       *out      = quaternion + (size_t) f * jointCount * 4;

        for (unsigned int j=0; j<jointCount; j++)
        {
            float translation[3], scale;
            arfDecomposeTRS(matrices + (size_t) j*16,translation,out + (size_t) j*4,&scale);
        }
    }

    /* Seed: a frame is suspect if the step into it or out of it is
     * implausible.  Deliberately generous -- requiring both sides would miss a
     * burst whose frames happen to resemble each other, and the refinement
     * below gives back everything this over-flags. */
    unsigned int flagged = 0;
    for (unsigned int f=0; f<frameCount; f++)
    {
        float into = (f > 0)
                   ? arfFrameDistance(quaternion + (size_t)(f-1) * jointCount * 4,
                                      quaternion + (size_t) f     * jointCount * 4,jointCount)
                   : 0.0f;
        float outOf = (f + 1 < frameCount)
                   ? arfFrameDistance(quaternion + (size_t) f     * jointCount * 4,
                                      quaternion + (size_t)(f+1) * jointCount * 4,jointCount)
                   : 0.0f;

        if ( (into > maxDegreesPerFrame) || (outOf > maxDegreesPerFrame) )
        {
            bad[f] = 1;
            flagged++;
        }
    }

    if (flagged==0) { goto done; }

    if (flagged == frameCount)
    {
        /* No clean frame anywhere to repair from -- this is a clip of noise,
         * not a clip with glitches in it, and interpolating would be a lie. */
        arfSetError("every frame exceeds the %.1f degree per frame threshold, refusing to despike",
                    (double) maxDegreesPerFrame);
        result = ARF_ERROR_ARGUMENT;
        goto done;
    }

    /* Refinement.  Two glitch bursts a few frames apart get seeded as one long
     * span, which would interpolate away the perfectly good dancing between
     * them.  So ask of every candidate: does interpolating across the span
     * actually disagree with what was recorded?  If not, the frame was never a
     * glitch -- give it back.  What survives is the frames that really do
     * depart from any smooth path through their neighbours. */
    for (unsigned int pass=0; pass<2; pass++)
    {
        unsigned int f = 0;
        while (f < frameCount)
        {
            if (!bad[f]) { f++; continue; }

            unsigned int spanStart = f;
            while ( (f < frameCount) && (bad[f]) ) { f++; }
            unsigned int spanEnd = f - 1;

            int low  = (int) spanStart - 1;
            int high = (int) spanEnd + 1;

            /* A span touching either end of the track has one anchor; using it
             * for both ends makes the test below "is this frame close to the
             * only clean pose we have", which is the right question there. */
            if ( (low < 0) && (high >= (int) frameCount) ) { continue; }
            if (low < 0)                    { low  = high; }
            if (high >= (int) frameCount)   { high = low;  }

            const float *lowQuaternion  = quaternion + (size_t) low  * jointCount * 4;
            const float *highQuaternion = quaternion + (size_t) high * jointCount * 4;

            for (unsigned int t=spanStart; t<=spanEnd; t++)
            {
                float alpha = (high > low) ? (float)((int) t - low) / (float)(high - low) : 0.0f;

                for (unsigned int j=0; j<jointCount; j++)
                {
                    arfQuaternionSlerp(candidate + (size_t) j*4,
                                       lowQuaternion  + (size_t) j*4,
                                       highQuaternion + (size_t) j*4,alpha);
                }

                if (arfFrameDistance(candidate,
                                     quaternion + (size_t) t * jointCount * 4,
                                     jointCount) <= maxDegreesPerFrame)
                {
                    bad[t] = 0;
                    flagged--;
                }
            }
        }
    }

    if (flagged==0) { goto done; }

    /* Anchors must come from the untouched track, so repairs are read from a
     * pristine copy and written into the live one. */
    original = (float *) malloc((size_t) frameCount * stride * sizeof(float));
    if (original==0)
    {
        arfSetError("out of memory copying %u frames to despike",frameCount);
        result = ARF_ERROR_MEMORY;
        goto done;
    }
    memcpy(original,avatar->localMatrices,(size_t) frameCount * stride * sizeof(float));

    unsigned int f = 0;
    while (f < frameCount)
    {
        if (!bad[f]) { f++; continue; }

        unsigned int spanStart = f;
        while ( (f < frameCount) && (bad[f]) ) { f++; }
        unsigned int spanEnd = f - 1;

        int low  = (int) spanStart - 1;
        int high = (int) spanEnd + 1;

        for (unsigned int t=spanStart; t<=spanEnd; t++)
        {
            if ( (low >= 0) && (high < (int) frameCount) )
            {
                float alpha = (float)((int) t - low) / (float)(high - low);
                arfInterpolateFrame(original,avatar->localMatrices,jointCount,
                                    t,(unsigned int) low,(unsigned int) high,alpha);
            }
            else
            {
                /* A glitch at the very start or end of a track has only one
                 * clean side; hold that pose rather than extrapolate. */
                unsigned int source = (low >= 0) ? (unsigned int) low : (unsigned int) high;
                memcpy(avatar->localMatrices + (size_t) t * stride,
                       original + (size_t) source * stride,
                       stride * sizeof(float));
            }
        }
    }

    if (repairedFrames!=0) { *repairedFrames = flagged; }

done:
    free(original);
    free(quaternion);
    free(bad);
    free(candidate);
    return result;
}
