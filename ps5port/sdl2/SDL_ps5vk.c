/* DevilutionX for PS5 - SDL2 video driver for the console's one display, shown through Vulkan (ps5vk.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console only has a full-screen output, so any window the game asks for (any size, resizable or not)
 * becomes that output. There is no OpenGL: pictures go to the screen through the render driver
 * (SDL_render_ps5vk.c). Joystick, audio and the event queue are SDL's own backends.
 */
#include "SDL_internal.h"
#include "video/SDL_sysvideo.h"
#include "events/SDL_keyboard_c.h"
#include "events/SDL_windowevents_c.h"

#include "ps5vk.h"

typedef struct {
    SDL_Window *window;
    int w, h;
} PS5VK_Video;

static int video_init(_THIS)
{
    PS5VK_Video *v = _this->driverdata;
    SDL_DisplayMode mode;
    double hz = 60.0;
    int index;
    if (ps5vk_open() < 0) {
        return SDL_SetError("PS5 Vulkan: %s", ps5vk_error());
    }
    ps5vk_size(&v->w, &v->h, &hz);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = v->w;
    mode.h = v->h;
    mode.refresh_rate = (int)(hz + 0.5);
    mode.driverdata = NULL;
    index = SDL_AddBasicVideoDisplay(&mode);
    if (index < 0) {
        return -1;
    }
    return SDL_AddDisplayMode(&_this->displays[index], &mode) ? 0 : -1;
}

static void video_quit(_THIS)
{
    (void)_this;
    ps5vk_close();
}

static int create_window(_THIS, SDL_Window *window)
{
    PS5VK_Video *v = _this->driverdata;
    if (v->window) {
        return SDL_SetError("PS5 supports one window");
    }
    /* the window is the full-screen output whatever size was asked for */
    window->x = window->windowed.x = 0;
    window->y = window->windowed.y = 0;
    window->w = window->windowed.w = v->w;
    window->h = window->windowed.h = v->h;
    window->flags |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS;
    window->flags &= ~SDL_WINDOW_RESIZABLE;
    v->window = window;
    SDL_SetKeyboardFocus(window); /* SDL also uses focus to gate joystick events. */
    /* a desktop window manager reports the size of a new window; the game reads its drawable size only from
     * this event (4:3 until it arrives) */
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_SIZE_CHANGED, window->w, window->h);
    return 0;
}

static void destroy_window(_THIS, SDL_Window *window)
{
    PS5VK_Video *v = _this->driverdata;
    if (v->window == window) {
        v->window = NULL;
    }
}

static void set_window_size(_THIS, SDL_Window *window)
{
    PS5VK_Video *v = _this->driverdata;
    window->w = window->windowed.w = v->w;
    window->h = window->windowed.h = v->h;
    SDL_SetError("PS5 SDL does not support resizing");
}

static void free_device(_THIS)
{
    SDL_free(_this->driverdata);
    SDL_free(_this);
}

/* Required SDL callback; joystick polling follows this in SDL_PumpEvents. */
static void pump_events(_THIS) { (void)_this; }

static SDL_VideoDevice *create_device(void)
{
    SDL_VideoDevice *device = SDL_calloc(1, sizeof(*device));
    if (!device) { SDL_OutOfMemory(); return NULL; }
    device->driverdata = SDL_calloc(1, sizeof(PS5VK_Video));
    if (!device->driverdata) { SDL_free(device); SDL_OutOfMemory(); return NULL; }
    device->VideoInit = video_init;
    device->VideoQuit = video_quit;
    device->PumpEvents = pump_events;
    device->CreateSDLWindow = create_window;
    device->DestroyWindow = destroy_window;
    device->SetWindowSize = set_window_size;
    device->free = free_device;
    return device;
}

VideoBootStrap PS5_bootstrap = { "ps5-vk", "PS5 Vulkan on VideoOut", create_device, NULL };
