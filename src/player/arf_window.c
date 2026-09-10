/** @file arf_window.c
 *  @brief GLX/X11 implementation of the window backend in arf_window.h.
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf_window.h"

#include <GL/glew.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct arfWindowX11
{
    Display   *display;
    Window     window;
    GLXContext context;
    Atom       deleteMessage;
    int        lastMouseX;
    int        lastMouseY;
    int        haveLastMouse;
};

typedef GLXContext (*glXCreateContextAttribsARBProc)(Display *, GLXFBConfig, GLXContext, Bool, const int *);

/** @brief Swallow the BadMatch that a driver without 3.3 core support raises
 *  from glXCreateContextAttribsARB, so the failure surfaces as a NULL context
 *  instead of killing the process inside Xlib's default handler. */
static int arfWindowIgnoreError(Display *display, XErrorEvent *event)
{
    (void) display;
    (void) event;
    return 0;
}

int arfWindowOpen(struct arfWindow *window, int width, int height, const char *title)
{
    memset(window,0,sizeof(struct arfWindow));

    struct arfWindowX11 *x11 = (struct arfWindowX11 *) calloc(1,sizeof(struct arfWindowX11));
    if (x11==0) { fprintf(stderr,"arfWindowOpen: out of memory\n"); return 0; }

    x11->display = XOpenDisplay(0);
    if (x11->display==0)
    {
        fprintf(stderr,"arfWindowOpen: cannot open an X display -- is DISPLAY set?\n");
        free(x11);
        return 0;
    }

    static const int visualAttributes[] =
    {
        GLX_X_RENDERABLE  , True,
        GLX_DRAWABLE_TYPE , GLX_WINDOW_BIT,
        GLX_RENDER_TYPE   , GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE , GLX_TRUE_COLOR,
        GLX_RED_SIZE      , 8,
        GLX_GREEN_SIZE    , 8,
        GLX_BLUE_SIZE     , 8,
        GLX_ALPHA_SIZE    , 8,
        GLX_DEPTH_SIZE    , 24,
        GLX_DOUBLEBUFFER  , True,
        None
    };

    int          configCount = 0;
    GLXFBConfig *configs = glXChooseFBConfig(x11->display,DefaultScreen(x11->display),visualAttributes,&configCount);

    if ( (configs==0) || (configCount==0) )
    {
        fprintf(stderr,"arfWindowOpen: no framebuffer config with RGBA8 and a 24 bit depth buffer\n");
        XCloseDisplay(x11->display);
        free(x11);
        return 0;
    }

    GLXFBConfig config = configs[0];
    XFree(configs);

    XVisualInfo *visual = glXGetVisualFromFBConfig(x11->display,config);
    if (visual==0)
    {
        fprintf(stderr,"arfWindowOpen: the chosen framebuffer config has no visual\n");
        XCloseDisplay(x11->display);
        free(x11);
        return 0;
    }

    Window rootWindow = RootWindow(x11->display,visual->screen);

    XSetWindowAttributes windowAttributes;
    memset(&windowAttributes,0,sizeof(windowAttributes));
    windowAttributes.colormap   = XCreateColormap(x11->display,rootWindow,visual->visual,AllocNone);
    windowAttributes.event_mask = ExposureMask | StructureNotifyMask |
                                  KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                                  PointerMotionMask;

    x11->window = XCreateWindow(x11->display,rootWindow,0,0,(unsigned int) width,(unsigned int) height,0,
                                visual->depth,InputOutput,visual->visual,
                                CWColormap | CWEventMask,&windowAttributes);
    XFree(visual);

    if (x11->window==0)
    {
        fprintf(stderr,"arfWindowOpen: XCreateWindow failed\n");
        XCloseDisplay(x11->display);
        free(x11);
        return 0;
    }

    XStoreName(x11->display,x11->window,title);
    XMapWindow(x11->display,x11->window);

    /* Ask the window manager to send a ClientMessage instead of killing the
     * connection when the close button is used, so the player can shut down
     * through its normal teardown path. */
    x11->deleteMessage = XInternAtom(x11->display,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(x11->display,x11->window,&x11->deleteMessage,1);

    glXCreateContextAttribsARBProc createContext =
        (glXCreateContextAttribsARBProc) glXGetProcAddressARB((const GLubyte *) "glXCreateContextAttribsARB");

    if (createContext!=0)
    {
        static const int contextAttributes[] =
        {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB , GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };

        int (*previousHandler)(Display *, XErrorEvent *) = XSetErrorHandler(arfWindowIgnoreError);
        x11->context = createContext(x11->display,config,0,True,contextAttributes);
        XSync(x11->display,False);
        XSetErrorHandler(previousHandler);
    }

    if (x11->context==0)
    {
        fprintf(stderr,"arfWindowOpen: could not create an OpenGL 3.3 core context\n");
        XDestroyWindow(x11->display,x11->window);
        XCloseDisplay(x11->display);
        free(x11);
        return 0;
    }

    glXMakeCurrent(x11->display,x11->window,x11->context);

    /* GLEW's core-profile path needs this, otherwise glewInit trips over a
     * glGetString(GL_EXTENSIONS) that a core context refuses to answer. */
    glewExperimental = GL_TRUE;
    GLenum glewStatus = glewInit();
    if (glewStatus != GLEW_OK)
    {
        fprintf(stderr,"arfWindowOpen: glewInit failed: %s\n",glewGetErrorString(glewStatus));
        glXMakeCurrent(x11->display,None,0);
        glXDestroyContext(x11->display,x11->context);
        XDestroyWindow(x11->display,x11->window);
        XCloseDisplay(x11->display);
        free(x11);
        return 0;
    }
    glGetError();  /* glewExperimental leaves a benign GL_INVALID_ENUM behind */

    window->width  = width;
    window->height = height;
    window->handle = x11;

    return 1;
}

/** @brief Map an X11 KeySym onto the small key vocabulary in arf_window.h. */
static int arfWindowTranslateKey(KeySym keysym)
{
    switch (keysym)
    {
        case XK_Escape : return ARF_KEY_ESCAPE;
        case XK_space  : return ARF_KEY_SPACE;
        case XK_Left   : return ARF_KEY_LEFT;
        case XK_Right  : return ARF_KEY_RIGHT;
        case XK_Up     : return ARF_KEY_UP;
        case XK_Down   : return ARF_KEY_DOWN;
        case XK_Home   : return ARF_KEY_HOME;
        case XK_End    : return ARF_KEY_END;
        default        : break;
    }

    if ( (keysym >= XK_A) && (keysym <= XK_Z) ) { return (int)(keysym - XK_A + 'a'); }
    if (keysym < 128)                           { return (int) keysym;               }

    return 0;
}

static void arfWindowQueueKey(struct arfWindow *window, int key)
{
    if (key==0)                                          { return; }
    if (window->keyCount >= ARF_WINDOW_KEY_QUEUE)        { return; }

    window->keys[window->keyCount++] = key;
}

int arfWindowPoll(struct arfWindow *window)
{
    struct arfWindowX11 *x11 = (struct arfWindowX11 *) window->handle;
    if (x11==0) { return 0; }

    window->keyCount   = 0;
    window->wheelDelta = 0;
    window->mouseDeltaX = 0;
    window->mouseDeltaY = 0;
    window->buttonPressed[0] = 0;
    window->buttonPressed[1] = 0;
    window->buttonPressed[2] = 0;

    while (XPending(x11->display) > 0)
    {
        XEvent event;
        XNextEvent(x11->display,&event);

        switch (event.type)
        {
            case ConfigureNotify:
                window->width  = event.xconfigure.width;
                window->height = event.xconfigure.height;
            break;

            case ClientMessage:
                if ((Atom) event.xclient.data.l[0] == x11->deleteMessage) { window->shouldClose = 1; }
            break;

            case KeyPress:
            {
                KeySym keysym = XLookupKeysym(&event.xkey,0);
                arfWindowQueueKey(window,arfWindowTranslateKey(keysym));
            }
            break;

            case ButtonPress:
            case ButtonRelease:
            {
                int pressed = (event.type==ButtonPress);

                /* X11 reports the wheel as buttons 4 and 5; count only the
                 * press half so one notch is one notch. */
                if (event.xbutton.button==Button4) { if (pressed) { window->wheelDelta += 1; } break; }
                if (event.xbutton.button==Button5) { if (pressed) { window->wheelDelta -= 1; } break; }

                int index = -1;
                if (event.xbutton.button==Button1) { index = ARF_MOUSE_LEFT;   }
                if (event.xbutton.button==Button2) { index = ARF_MOUSE_MIDDLE; }
                if (event.xbutton.button==Button3) { index = ARF_MOUSE_RIGHT;  }

                if (index >= 0)
                {
                    window->buttonDown[index] = pressed;
                    if (pressed) { window->buttonPressed[index] = 1; }

                    window->mouseX = event.xbutton.x;
                    window->mouseY = event.xbutton.y;
                    x11->lastMouseX = window->mouseX;
                    x11->lastMouseY = window->mouseY;
                    x11->haveLastMouse = 1;
                }
            }
            break;

            case MotionNotify:
                window->mouseX = event.xmotion.x;
                window->mouseY = event.xmotion.y;

                if (x11->haveLastMouse)
                {
                    window->mouseDeltaX += window->mouseX - x11->lastMouseX;
                    window->mouseDeltaY += window->mouseY - x11->lastMouseY;
                }

                x11->lastMouseX = window->mouseX;
                x11->lastMouseY = window->mouseY;
                x11->haveLastMouse = 1;
            break;

            default:
            break;
        }
    }

    return (window->shouldClose==0);
}

void arfWindowSwap(struct arfWindow *window)
{
    struct arfWindowX11 *x11 = (struct arfWindowX11 *) window->handle;
    if (x11!=0) { glXSwapBuffers(x11->display,x11->window); }
}

void arfWindowSetTitle(struct arfWindow *window, const char *title)
{
    struct arfWindowX11 *x11 = (struct arfWindowX11 *) window->handle;
    if (x11!=0) { XStoreName(x11->display,x11->window,title); }
}

void arfWindowClose(struct arfWindow *window)
{
    struct arfWindowX11 *x11 = (struct arfWindowX11 *) window->handle;
    if (x11==0) { return; }

    glXMakeCurrent(x11->display,None,0);
    glXDestroyContext(x11->display,x11->context);
    XDestroyWindow(x11->display,x11->window);
    XCloseDisplay(x11->display);

    free(x11);
    window->handle = 0;
}

double arfWindowTime(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (double) now.tv_sec + (double) now.tv_nsec / 1000000000.0;
}
