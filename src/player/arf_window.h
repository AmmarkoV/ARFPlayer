/** @file arf_window.h
 *  @brief A minimal GLX/X11 window with an OpenGL 3.3 core context.
 *
 *  Everything platform specific in this player lives behind this header: open
 *  a window, poll input, swap buffers, close.  Input is exposed as plain
 *  per-frame state rather than callbacks, which is what a player's single
 *  render loop actually wants, and it keeps X11 out of every other file.
 *
 *  Porting to GLFW means reimplementing these six functions, nothing else.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_WINDOW_H_INCLUDED
#define ARF_WINDOW_H_INCLUDED

/** Keys that have no printable character.  Printable keys arrive as their
 *  lowercase ASCII code, so 'a' is 'a' and space is 32. */
#define ARF_KEY_ESCAPE     27
#define ARF_KEY_SPACE      32
#define ARF_KEY_LEFT       0x100
#define ARF_KEY_RIGHT      0x101
#define ARF_KEY_UP         0x102
#define ARF_KEY_DOWN       0x103
#define ARF_KEY_HOME       0x104
#define ARF_KEY_END        0x105

#define ARF_MOUSE_LEFT     0
#define ARF_MOUSE_MIDDLE   1
#define ARF_MOUSE_RIGHT    2

/** @brief How many key presses one poll can report before dropping them. */
#define ARF_WINDOW_KEY_QUEUE 16

/** @brief Window and input state.  Everything below `handle` is refreshed by
 *  arfWindowPoll() and is valid until the next call to it. */
struct arfWindow
{
    int width;
    int height;
    int shouldClose;

    int mouseX;             /**< pixels, origin top left */
    int mouseY;
    int mouseDeltaX;        /**< movement since the previous poll */
    int mouseDeltaY;
    int buttonDown[3];      /**< ARF_MOUSE_*, held state */
    int buttonPressed[3];   /**< ARF_MOUSE_*, went down during this poll */
    int wheelDelta;         /**< notches this poll, positive is scroll up */

    int keyCount;
    int keys[ARF_WINDOW_KEY_QUEUE];  /**< keys pressed during this poll */

    void *handle;           /**< opaque X11/GLX state */
};

/** @brief Open a window and make its OpenGL 3.3 core context current.
 *  @retval 1 on success, 0 on failure with a message already on stderr */
int arfWindowOpen(struct arfWindow *window, int width, int height, const char *title);

/** @brief Drain pending events into the window's input state.
 *  @retval 1 while the window should keep running, 0 once a close was asked for */
int arfWindowPoll(struct arfWindow *window);

/** @brief Present the back buffer. */
void arfWindowSwap(struct arfWindow *window);

/** @brief Replace the title bar text. */
void arfWindowSetTitle(struct arfWindow *window, const char *title);

/** @brief Destroy the context and the window. */
void arfWindowClose(struct arfWindow *window);

/** @brief Monotonic seconds, for pacing playback.  Unrelated to the window,
 *  but every backend has to provide one anyway. */
double arfWindowTime(void);

#endif /* ARF_WINDOW_H_INCLUDED */
