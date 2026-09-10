/** @file arfplay.c
 *  @brief The ARF player: command line front end, playback clock and timeline.
 *
 *  Everything here is presentation.  Parsing lives in libarf, skinning lives in
 *  libarf, the window lives behind arf_window.h -- this file decides which
 *  frame should be on screen right now and draws it.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "../libarf/arf.h"
#include "arf_window.h"
#include "arf_camera.h"
#include "arf_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef ARF_SHADER_DIR
#define ARF_SHADER_DIR "shaders"
#endif

/** @brief Fraction of the window height the timeline strip occupies. */
#define ARF_TIMELINE_HEIGHT 0.055f

struct arfOptions
{
    const char *filename;
    const char *shaderDirectory;
    const char *exportOBJ;
    const char *saveAs;
    int         infoOnly;
    long        exportFrame;      /**< -1 means the rest pose */
    int         width;
    int         height;
    float       color[3];
    float       speed;
    int         loop;
    int         despike;          /**< -1 unset, 0 off, 1 on */
    float       despikeDegrees;
};

static void arfPrintUsage(void)
{
    printf(
    "arfplay -- play an MPEG ARF avatar container (.arfz)\n"
    "\n"
    "usage: arfplay [options] <container.arfz>\n"
    "\n"
    "options:\n"
    "  --info                 print a summary of the container and exit\n"
    "  --export-obj <file>    write the mesh as a Wavefront OBJ and exit\n"
    "  --frame <n>            frame to pose for --export-obj (default: the rest mesh)\n"
    "  --save <file>          re-encode the container and exit, for round trip checks\n"
    "  --shaders <dir>        where default.vert and default.frag live\n"
    "  --width <n>            window width, default 1280\n"
    "  --height <n>           window height, default 720\n"
    "  --color <r,g,b>        mesh tint, 0 to 1 per channel, default 0.8,0.75,0.7\n"
    "  --speed <x>            initial playback rate, default 1.0\n"
    "  --no-loop              stop at the end instead of looping\n"
    "  --despike[=<deg>]      repair tracking-glitch frames by interpolating across\n"
    "                         them (default on when viewing, off for --info,\n"
    "                         --save and --export-obj; threshold %.0f deg/frame)\n"
    "  --no-despike           show the recorded animation exactly as stored\n"
    "  --help                 this text\n"
    "\n"
    "controls:\n"
    "  space                  play / pause\n"
    "  left, right            step one frame\n"
    "  home, end              jump to the first or last frame\n"
    "  l                      toggle looping\n"
    "  [ , ]                  halve or double the playback speed\n"
    "  r                      reset the camera\n"
    "  escape                 quit\n"
    "  left drag              orbit          middle or right drag   pan\n"
    "  wheel                  zoom           drag the timeline      scrub\n"
    "\n"
    "This reads SAM3DBody-flavoured ARF and is not a certified conformant\n"
    "ISO/IEC 23090-39 implementation.  See README.md.\n",
    (double) ARF_DESPIKE_DEFAULT_DEGREES);
}

/** @brief Pick the shader directory: an explicit flag wins, otherwise a
 *  shaders/ next to the working directory, otherwise the configured default. */
static const char *arfResolveShaderDirectory(const char *requested)
{
    if (requested!=0) { return requested; }

    FILE *probe = fopen("shaders/default.vert","rb");
    if (probe!=0) { fclose(probe); return "shaders"; }

    return ARF_SHADER_DIR;
}

static int arfParseOptions(int argc, char **argv, struct arfOptions *options)
{
    memset(options,0,sizeof(struct arfOptions));
    options->width       = 1280;
    options->height      = 720;
    options->speed       = 1.0f;
    options->loop        = 1;
    options->exportFrame = -1;
    options->despike     = -1;
    options->despikeDegrees = ARF_DESPIKE_DEFAULT_DEGREES;
    options->color[0] = 0.80f;
    options->color[1] = 0.75f;
    options->color[2] = 0.70f;

    for (int i=1; i<argc; i++)
    {
        const char *argument = argv[i];
        int         hasValue = (i+1 < argc);

        if      (strcmp(argument,"--help")==0)                    { arfPrintUsage(); return 0; }
        else if (strcmp(argument,"--info")==0)                    { options->infoOnly = 1; }
        else if (strcmp(argument,"--no-loop")==0)                 { options->loop = 0; }
        else if (strcmp(argument,"--no-despike")==0)              { options->despike = 0; }
        else if (strcmp(argument,"--despike")==0)                 { options->despike = 1; }
        else if (strncmp(argument,"--despike=",10)==0)
        {
            options->despike = 1;
            options->despikeDegrees = (float) atof(argument + 10);
            if (!(options->despikeDegrees > 0.0f))
            {
                fprintf(stderr,"--despike wants a positive threshold in degrees per frame\n");
                return 0;
            }
        }
        else if ((strcmp(argument,"--export-obj")==0) && hasValue) { options->exportOBJ = argv[++i]; }
        else if ((strcmp(argument,"--save")==0)       && hasValue) { options->saveAs = argv[++i]; }
        else if ((strcmp(argument,"--shaders")==0)    && hasValue) { options->shaderDirectory = argv[++i]; }
        else if ((strcmp(argument,"--frame")==0)      && hasValue) { options->exportFrame = strtol(argv[++i],0,10); }
        else if ((strcmp(argument,"--width")==0)      && hasValue) { options->width  = (int) strtol(argv[++i],0,10); }
        else if ((strcmp(argument,"--height")==0)     && hasValue) { options->height = (int) strtol(argv[++i],0,10); }
        else if ((strcmp(argument,"--speed")==0)      && hasValue) { options->speed  = (float) atof(argv[++i]); }
        else if ((strcmp(argument,"--color")==0)      && hasValue)
        {
            if (sscanf(argv[++i],"%f,%f,%f",&options->color[0],&options->color[1],&options->color[2]) != 3)
            {
                fprintf(stderr,"--color wants three comma separated numbers, for example 0.8,0.75,0.7\n");
                return 0;
            }
        }
        else if (argument[0]=='-')
        {
            fprintf(stderr,"unknown option \"%s\", try --help\n",argument);
            return 0;
        }
        else { options->filename = argument; }
    }

    if (options->filename==0)
    {
        fprintf(stderr,"no container was given, try --help\n");
        return 0;
    }

    if (options->speed <= 0.0f) { options->speed = 1.0f; }

    return 1;
}

/** @brief Work out where to point the camera at this clip.
 *
 *  The centre comes from the union of every sampled pose, so a walking avatar
 *  stays in frame without the camera having to follow it; the radius comes
 *  from the largest single pose, so one pose fills the view rather than the
 *  whole travel volume, which for a clip with real locomotion is several times
 *  larger than the person. */
static void arfClipFraming(struct arfAvatar *avatar, struct arfPose *pose, float *centre, float *radius)
{
    float unionMinimum[3] = {  1e30f,  1e30f,  1e30f };
    float unionMaximum[3] = { -1e30f, -1e30f, -1e30f };
    float largest = 0.0f;

    /* Sampling rather than evaluating every frame: a couple of dozen poses
     * bound a human motion closely enough to aim a camera, and a long clip
     * would otherwise cost a full skinning pass per frame at startup. */
    unsigned int step = (avatar->numberOfFrames + 23) / 24;
    if (step==0) { step = 1; }

    for (unsigned int frame=0; frame<avatar->numberOfFrames; frame+=step)
    {
        arfPoseEvaluate(avatar,pose,frame);

        float minimum[3] = {  1e30f,  1e30f,  1e30f };
        float maximum[3] = { -1e30f, -1e30f, -1e30f };

        for (unsigned int v=0; v<pose->numberOfVertices; v++)
        {
            for (unsigned int c=0; c<3; c++)
            {
                float value = pose->positions[v*3+c];
                if (value < minimum[c]) { minimum[c] = value; }
                if (value > maximum[c]) { maximum[c] = value; }
            }
        }

        float poseRadius = 0.0f;
        for (unsigned int c=0; c<3; c++)
        {
            if (minimum[c] < unionMinimum[c]) { unionMinimum[c] = minimum[c]; }
            if (maximum[c] > unionMaximum[c]) { unionMaximum[c] = maximum[c]; }

            float half = 0.5f * (maximum[c] - minimum[c]);
            poseRadius += half * half;
        }

        poseRadius = sqrtf(poseRadius);
        if (poseRadius > largest) { largest = poseRadius; }
    }

    for (unsigned int c=0; c<3; c++) { centre[c] = 0.5f * (unionMinimum[c] + unionMaximum[c]); }
    *radius = largest;
}

/** @brief Map a mouse x in pixels onto a frame index. */
static unsigned int arfFrameFromScrub(const struct arfAvatar *avatar, int mouseX, int windowWidth)
{
    if (windowWidth <= 1) { return 0; }

    float fraction = (float) mouseX / (float) (windowWidth - 1);
    if (fraction < 0.0f) { fraction = 0.0f; }
    if (fraction > 1.0f) { fraction = 1.0f; }

    unsigned int frame = (unsigned int)(fraction * (float)(avatar->numberOfFrames - 1) + 0.5f);
    if (frame >= avatar->numberOfFrames) { frame = avatar->numberOfFrames - 1; }

    return frame;
}

static void arfDrawTimeline(struct arfRenderer *renderer, unsigned int frame, unsigned int frameCount)
{
    float progress = (frameCount > 1) ? (float) frame / (float)(frameCount - 1) : 0.0f;
    float top      = ARF_TIMELINE_HEIGHT;

    arfRendererDrawRect(renderer,0.0f,0.0f,1.0f,top,           0.06f,0.07f,0.09f,0.85f);
    arfRendererDrawRect(renderer,0.0f,0.0f,progress,top,       0.25f,0.55f,0.85f,0.85f);

    /* A playhead a couple of pixels wide, kept inside the strip at both ends. */
    float half = 0.0018f;
    float left  = progress - half;
    float right = progress + half;
    if (left  < 0.0f) { left  = 0.0f; right = 2.0f*half; }
    if (right > 1.0f) { right = 1.0f; left  = 1.0f - 2.0f*half; }

    arfRendererDrawRect(renderer,left,0.0f,right,top,          0.95f,0.95f,0.95f,1.0f);
}

int main(int argc, char **argv)
{
    struct arfOptions options;
    if (!arfParseOptions(argc,argv,&options)) { return 1; }

    struct arfAvatar *avatar = arfLoad(options.filename);
    if (avatar==0)
    {
        fprintf(stderr,"cannot load \"%s\": %s\n",options.filename,arfLastError());
        return 1;
    }

    if (options.infoOnly)
    {
        arfPrintInfo(avatar);

        /* Reported rather than applied: --info describes what is in the file.
         * The repair runs on this throwaway copy only to count. */
        unsigned int repaired = 0;
        if ( (options.despike!=0) &&
             (arfDespikeFrames(avatar,options.despikeDegrees,&repaired)==ARF_OK) )
        {
            printf("glitch frames : %u of %u exceed %.0f deg/frame",
                   repaired,avatar->numberOfFrames,(double) options.despikeDegrees);
            printf("%s\n",(repaired>0) ? " -- --despike interpolates across them" : "");
        }

        arfFree(avatar);
        return 0;
    }

    /* Repairing rewrites the animation, so it is on by default only for
     * viewing.  Anything that writes data back out gets the recorded track
     * unless the repair was asked for explicitly. */
    if ( (options.despike > 0) ||
         ((options.despike < 0) && (options.saveAs==0) && (options.exportOBJ==0)) )
    {
        unsigned int repaired = 0;
        if (arfDespikeFrames(avatar,options.despikeDegrees,&repaired)!=ARF_OK)
        {
            fprintf(stderr,"could not despike: %s\n",arfLastError());
        }
        else if (repaired > 0)
        {
            printf("repaired %u glitch frame(s) above %.0f deg/frame -- --no-despike to see them as recorded\n",
                   repaired,(double) options.despikeDegrees);
        }
    }

    if (options.saveAs!=0)
    {
        int result = arfSave(avatar,options.saveAs);
        if (result!=ARF_OK) { fprintf(stderr,"cannot write \"%s\": %s\n",options.saveAs,arfLastError()); }
        else                { printf("wrote %s\n",options.saveAs); }

        arfFree(avatar);
        return (result==ARF_OK) ? 0 : 1;
    }

    struct arfPose *pose = arfPoseAllocate(avatar);
    if (pose==0)
    {
        fprintf(stderr,"cannot allocate a pose: %s\n",arfLastError());
        arfFree(avatar);
        return 1;
    }

    if (options.exportOBJ!=0)
    {
        const float *positions = 0;

        if (options.exportFrame >= 0)
        {
            if (arfPoseEvaluate(avatar,pose,(unsigned int) options.exportFrame)!=ARF_OK)
            {
                fprintf(stderr,"%s\n",arfLastError());
                arfPoseFree(pose);
                arfFree(avatar);
                return 1;
            }
            positions = pose->positions;
        }

        int result = arfExportOBJ(avatar,positions,options.exportOBJ);
        if (result!=ARF_OK) { fprintf(stderr,"cannot write \"%s\": %s\n",options.exportOBJ,arfLastError()); }
        else                { printf("wrote %s (%u vertices, %u triangles)\n",options.exportOBJ,
                                     avatar->mesh.numberOfVertices,avatar->mesh.numberOfTriangles); }

        arfPoseFree(pose);
        arfFree(avatar);
        return (result==ARF_OK) ? 0 : 1;
    }

    struct arfWindow window;
    if (!arfWindowOpen(&window,options.width,options.height,"arfplay"))
    {
        arfPoseFree(pose);
        arfFree(avatar);
        return 1;
    }

    struct arfRenderer *renderer = arfRendererCreate(avatar,arfResolveShaderDirectory(options.shaderDirectory));
    if (renderer==0)
    {
        arfWindowClose(&window);
        arfPoseFree(pose);
        arfFree(avatar);
        return 1;
    }

    float clipCentre[3];
    float clipRadius = 0.0f;
    arfClipFraming(avatar,pose,clipCentre,&clipRadius);

    struct arfCamera camera;
    arfCameraReset(&camera,clipCentre,clipRadius);

    unsigned int frame     = 0;
    int          playing   = 1;
    int          looping   = options.loop;
    int          scrubbing = 0;
    float        speed     = options.speed;

    /* Playback position is kept as a float frame index advanced by wall clock
     * time, so the clip runs at its own timescale regardless of the render
     * rate, and a dropped frame costs no drift. */
    double playhead      = 0.0;
    double previousTime  = arfWindowTime();
    double titleDeadline = 0.0;
    unsigned int drawnFrames = 0;
    double frameRateWindow  = previousTime;
    float  measuredFPS      = 0.0f;
    unsigned int lastEvaluated = avatar->numberOfFrames;  /* nothing evaluated yet */

    while (arfWindowPoll(&window))
    {
        double now     = arfWindowTime();
        double elapsed = now - previousTime;
        previousTime   = now;

        for (int k=0; k<window.keyCount; k++)
        {
            switch (window.keys[k])
            {
                case ARF_KEY_ESCAPE : window.shouldClose = 1; break;
                case ARF_KEY_SPACE  : playing = !playing; playhead = (double) frame; break;
                case 'l'            : looping = !looping; break;
                case '['            : speed *= 0.5f; if (speed < 0.03125f) { speed = 0.03125f; } break;
                case ']'            : speed *= 2.0f; if (speed > 16.0f)    { speed = 16.0f;    } break;
                case 'r'            : arfCameraReset(&camera,clipCentre,clipRadius); break;

                /* Stepping and jumping re-anchor the clock on the new frame,
                 * so resuming playback does not snap back to where the wall
                 * clock had got to. */
                case ARF_KEY_LEFT   : playing = 0; if (frame > 0) { frame--; } playhead = (double) frame; break;
                case ARF_KEY_RIGHT  : playing = 0; if (frame + 1 < avatar->numberOfFrames) { frame++; } playhead = (double) frame; break;
                case ARF_KEY_HOME   : frame = 0; playhead = 0.0; break;
                case ARF_KEY_END    : frame = avatar->numberOfFrames - 1; playhead = (double) frame; break;

                default             : break;
            }
        }

        /* The timeline strip owns the bottom of the window; a press anywhere
         * in it starts a scrub that continues until the button is released. */
        int timelineTop = (int)((float) window.height * ARF_TIMELINE_HEIGHT);
        if ( (window.buttonPressed[ARF_MOUSE_LEFT]) && (window.mouseY >= window.height - timelineTop) )
        {
            scrubbing = 1;
            playing   = 0;
        }
        if (!window.buttonDown[ARF_MOUSE_LEFT]) { scrubbing = 0; }

        if (scrubbing)
        {
            frame    = arfFrameFromScrub(avatar,window.mouseX,window.width);
            playhead = (double) frame;
        }
        else
        {
            if (window.buttonDown[ARF_MOUSE_LEFT])
            {
                arfCameraOrbit(&camera,(float) window.mouseDeltaX * 0.007f,
                                       (float) window.mouseDeltaY * 0.007f);
            }
            if ( (window.buttonDown[ARF_MOUSE_MIDDLE]) || (window.buttonDown[ARF_MOUSE_RIGHT]) )
            {
                arfCameraPan(&camera,(float) window.mouseDeltaX,(float) window.mouseDeltaY,window.height);
            }
        }

        if (window.wheelDelta != 0)
        {
            arfCameraZoom(&camera,powf(0.88f,(float) window.wheelDelta));
        }

        if (playing)
        {
            playhead += elapsed * (double) avatar->timescale * (double) speed;

            if (playhead >= (double) avatar->numberOfFrames)
            {
                if (looping) { playhead = fmod(playhead,(double) avatar->numberOfFrames); }
                else         { playhead = (double) avatar->numberOfFrames - 1; playing = 0; }
            }

            frame = (unsigned int) playhead;
            if (frame >= avatar->numberOfFrames) { frame = avatar->numberOfFrames - 1; }
        }

        if (frame != lastEvaluated)
        {
            arfPoseEvaluate(avatar,pose,frame);
            arfRendererUpload(renderer,pose);
            lastEvaluated = frame;
        }

        float aspect = (window.height > 0) ? (float) window.width / (float) window.height : 1.0f;

        float projection[16];
        float view[16];
        float modelViewProjection[16];

        arfCameraProjectionMatrix(&camera,aspect,projection);
        arfCameraViewMatrix(&camera,view);
        arfMultiply4x4(modelViewProjection,projection,view);

        arfRendererBeginFrame(window.width,window.height);
        arfRendererDrawMesh(renderer,modelViewProjection,view,options.color);
        arfDrawTimeline(renderer,frame,avatar->numberOfFrames);
        arfWindowSwap(&window);

        drawnFrames++;
        if (now - frameRateWindow >= 0.5)
        {
            measuredFPS     = (float)((double) drawnFrames / (now - frameRateWindow));
            drawnFrames     = 0;
            frameRateWindow = now;
        }

        if (now >= titleDeadline)
        {
            char title[256];
            snprintf(title,sizeof(title),
                     "arfplay  |  %s  |  frame %u/%u  %.2f/%.2f s  |  %s  x%.2f%s  |  %.0f fps",
                     avatar->id,frame,avatar->numberOfFrames-1,
                     (float) frame / avatar->timescale,arfDuration(avatar),
                     playing ? "playing" : "paused",(double) speed,looping ? "  loop" : "",
                     (double) measuredFPS);
            arfWindowSetTitle(&window,title);
            titleDeadline = now + 0.25;
        }
    }

    arfRendererDestroy(renderer);
    arfWindowClose(&window);
    arfPoseFree(pose);
    arfFree(avatar);
    return 0;
}
