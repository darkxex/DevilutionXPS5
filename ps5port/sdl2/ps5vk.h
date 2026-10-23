/*
 * DevilutionX for PS5 - the screen through Vulkan (RADV, VK_KHR_display on VideoOut).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What SDL's render driver needs to put the game's software-rendered picture on the TV: one display
 * surface and swapchain, textures uploaded through staging buffers (the GPU does the tiling, not the
 * CPU), and a frame made of a clear colour and textured rectangles.
 */
#ifndef PS5VK_H
#define PS5VK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PS5VKTexture PS5VKTexture;

/* The mode the app would like (e.g. 2560x1440); the closest the TV offers at about 60 Hz is used.
 * Call before SDL starts. 0 means any. */
void ps5vk_prefer_mode(int width, int height);

/* Opens the display. 0, or -1 with ps5vk_error(). Calling it again once open does nothing. */
int ps5vk_open(void);
const char *ps5vk_error(void);
/* The swapchain's size and the mode's refresh rate. */
void ps5vk_size(int *width, int *height, double *hz);
void ps5vk_close(void);

/* bgra: the pixels are B,G,R,A bytes (SDL's ARGB8888), else R,G,B,A. */
PS5VKTexture *ps5vk_texture_create(int width, int height, int bgra);
void ps5vk_texture_destroy(PS5VKTexture *texture);
/* Copies a rectangle of the texture's pixels into this frame's staging buffer; the GPU copy is recorded by
 * ps5vk_frame_present. pitch is the source's row length in bytes. */
void ps5vk_texture_update(PS5VKTexture *texture, int x, int y, int width, int height, const void *pixels, int pitch);

/* The frame in the making: the colour it is cleared to, then the rectangles drawn over it (destination in
 * screen pixels, source in 0..1 texture coordinates). linear: bilinear filter, else nearest. */
void ps5vk_frame_clear(float r, float g, float b);
void ps5vk_frame_draw(PS5VKTexture *texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                      float v1, int linear);
/* Records the frame (uploads, clear, draws), submits it and queues it for the next vblank. */
void ps5vk_frame_present(void);

/* A line to the console's klog (nc <PS5 IP> 3232). */
void ps5vk_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
