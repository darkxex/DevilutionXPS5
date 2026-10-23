/* DevilutionX for PS5 - SDL2 render driver on Vulkan (ps5vk.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The game draws in software and puts one texture on the screen each frame: a clear and a textured rectangle.
 * That is all this driver draws. SDL queues copies as geometry, so a rectangle is the bounding box of the
 * vertices it was queued with; points, lines, fills and render targets are not supported.
 */
#include "SDL_internal.h"

#if defined(SDL_VIDEO_RENDER_PS5VK) && !defined(SDL_RENDER_DISABLED)

#include "render/SDL_sysrender.h"
#include "SDL_hints.h"

#include "ps5vk.h"

extern SDL_RenderDriver PS5VK_RenderDriver;

typedef struct
{
    PS5VKTexture *texture;
    int bgra;
    void *pixels; /* streaming textures: the pixels SDL_LockTexture hands out */
    int pitch;
    SDL_Rect locked;
} PS5VK_TextureData;

static void PS5VK_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    (void)renderer;
    (void)event;
}

static int PS5VK_GetOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
    SDL_GetWindowSize(renderer->window, w, h);
    return 0;
}

static SDL_bool PS5VK_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    (void)renderer;
    (void)blendMode;
    return SDL_TRUE; /* the screen is opaque: the blend mode is ignored */
}

static int PS5VK_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    PS5VK_TextureData *data;
    (void)renderer;
    if (texture->format != SDL_PIXELFORMAT_ABGR8888 && texture->format != SDL_PIXELFORMAT_ARGB8888) {
        return SDL_SetError("PS5VK: unsupported texture format");
    }
    if (texture->access == SDL_TEXTUREACCESS_TARGET) {
        return SDL_SetError("PS5VK: render targets are not supported");
    }
    data = (PS5VK_TextureData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    data->bgra = texture->format == SDL_PIXELFORMAT_ARGB8888;
    data->texture = ps5vk_texture_create(texture->w, texture->h, data->bgra);
    if (!data->texture) {
        SDL_free(data);
        return SDL_SetError("PS5VK: %s", ps5vk_error());
    }
    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        data->pitch = texture->w * 4;
        data->pixels = SDL_calloc(texture->h, data->pitch);
        if (!data->pixels) {
            ps5vk_texture_destroy(data->texture);
            SDL_free(data);
            return SDL_OutOfMemory();
        }
    }
    texture->driverdata = data;
    return 0;
}

static int PS5VK_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect,
                               const void *pixels, int pitch)
{
    PS5VK_TextureData *data = (PS5VK_TextureData *)texture->driverdata;
    (void)renderer;
    ps5vk_texture_update(data->texture, rect->x, rect->y, rect->w, rect->h, pixels, pitch);
    return 0;
}

static int PS5VK_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels,
                             int *pitch)
{
    PS5VK_TextureData *data = (PS5VK_TextureData *)texture->driverdata;
    (void)renderer;
    if (!data->pixels) {
        return SDL_SetError("PS5VK: only streaming textures can be locked");
    }
    data->locked = *rect;
    *pixels = (Uint8 *)data->pixels + rect->y * data->pitch + rect->x * 4;
    *pitch = data->pitch;
    return 0;
}

static void PS5VK_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    PS5VK_TextureData *data = (PS5VK_TextureData *)texture->driverdata;
    (void)renderer;
    ps5vk_texture_update(data->texture, data->locked.x, data->locked.y, data->locked.w, data->locked.h,
                         (Uint8 *)data->pixels + data->locked.y * data->pitch + data->locked.x * 4, data->pitch);
}

static void PS5VK_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
    (void)renderer;
    (void)texture;
    (void)scaleMode; /* read from the texture when it is drawn */
}

static int PS5VK_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    (void)renderer;
    return texture ? SDL_SetError("PS5VK: render targets are not supported") : 0;
}

static int PS5VK_QueueNoop(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    (void)renderer;
    (void)cmd;
    return 0; /* viewport and draw colour are read from the command when it runs */
}

static int PS5VK_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    (void)renderer;
    (void)points;
    (void)count;
    cmd->data.draw.count = 0; /* not drawn */
    return 0;
}

static int PS5VK_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const float *xy,
                               int xy_stride, const SDL_Color *color, int color_stride, const float *uv,
                               int uv_stride, int num_vertices, const void *indices, int num_indices,
                               int size_indices, float scale_x, float scale_y)
{
    int count = indices ? num_indices : num_vertices;
    int i;
    SDL_Vertex *verts;
    size_indices = indices ? size_indices : 0;
    cmd->data.draw.count = 0;
    if (!texture) {
        return 0; /* fills are not drawn */
    }
    verts = (SDL_Vertex *)SDL_AllocateRenderVertices(renderer, count * sizeof(*verts), 0, &cmd->data.draw.first);
    if (!verts) {
        return -1;
    }
    cmd->data.draw.count = count;
    for (i = 0; i < count; i++) {
        int j = i;
        const float *xy_;
        const float *uv_;
        if (size_indices == 4) {
            j = ((const Uint32 *)indices)[i];
        } else if (size_indices == 2) {
            j = ((const Uint16 *)indices)[i];
        } else if (size_indices == 1) {
            j = ((const Uint8 *)indices)[i];
        }
        xy_ = (const float *)((const char *)xy + j * xy_stride);
        uv_ = (const float *)((const char *)uv + j * uv_stride);
        verts[i].position.x = xy_[0] * scale_x;
        verts[i].position.y = xy_[1] * scale_y;
        verts[i].color = *(const SDL_Color *)((const char *)color + j * color_stride);
        verts[i].tex_coord.x = uv_[0];
        verts[i].tex_coord.y = uv_[1];
    }
    return 0;
}

static int PS5VK_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    SDL_Rect viewport = { 0, 0, 0, 0 };
    (void)renderer;
    (void)vertsize;

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETVIEWPORT:
            viewport = cmd->data.viewport.rect;
            break;

        case SDL_RENDERCMD_CLEAR:
            ps5vk_frame_clear(cmd->data.color.r / 255.0f, cmd->data.color.g / 255.0f, cmd->data.color.b / 255.0f);
            break;

        case SDL_RENDERCMD_GEOMETRY: {
            SDL_Texture *texture = cmd->data.draw.texture;
            size_t count = cmd->data.draw.count;
            const SDL_Vertex *verts = (const SDL_Vertex *)((const Uint8 *)vertices + cmd->data.draw.first);
            float x0, y0, x1, y1, u0, v0, u1, v1;
            size_t i;
            if (!texture || count < 3) {
                break;
            }
            x0 = x1 = verts[0].position.x;
            y0 = y1 = verts[0].position.y;
            u0 = u1 = verts[0].tex_coord.x;
            v0 = v1 = verts[0].tex_coord.y;
            for (i = 1; i < count; i++) {
                if (verts[i].position.x < x0) x0 = verts[i].position.x;
                if (verts[i].position.x > x1) x1 = verts[i].position.x;
                if (verts[i].position.y < y0) y0 = verts[i].position.y;
                if (verts[i].position.y > y1) y1 = verts[i].position.y;
                if (verts[i].tex_coord.x < u0) u0 = verts[i].tex_coord.x;
                if (verts[i].tex_coord.x > u1) u1 = verts[i].tex_coord.x;
                if (verts[i].tex_coord.y < v0) v0 = verts[i].tex_coord.y;
                if (verts[i].tex_coord.y > v1) v1 = verts[i].tex_coord.y;
            }
            ps5vk_frame_draw(((PS5VK_TextureData *)texture->driverdata)->texture, viewport.x + x0, viewport.y + y0,
                             viewport.x + x1, viewport.y + y1, u0, v0, u1, v1,
                             texture->scaleMode != SDL_ScaleModeNearest);
            break;
        }

        default: /* clip rectangles, draw colours, points, lines and fills are not used */
            break;
        }
        cmd = cmd->next;
    }
    return 0;
}

static int PS5VK_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect, Uint32 format, void *pixels, int pitch)
{
    (void)renderer;
    (void)rect;
    (void)format;
    (void)pixels;
    (void)pitch;
    return SDL_Unsupported();
}

static int PS5VK_RenderPresent(SDL_Renderer *renderer)
{
    (void)renderer;
    ps5vk_frame_present();
    return 0;
}

static void PS5VK_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    PS5VK_TextureData *data = (PS5VK_TextureData *)texture->driverdata;
    (void)renderer;
    if (data) {
        ps5vk_texture_destroy(data->texture);
        SDL_free(data->pixels);
        SDL_free(data);
        texture->driverdata = NULL;
    }
}

static void PS5VK_DestroyRenderer(SDL_Renderer *renderer)
{
    SDL_free(renderer); /* the display belongs to the video driver */
}

static int PS5VK_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    (void)renderer;
    (void)vsync;
    return 0; /* always the TV's vblank (FIFO) */
}

static SDL_Renderer *PS5VK_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    SDL_Renderer *renderer;
    int w = 0, h = 0;

    if (flags & SDL_RENDERER_SOFTWARE) {
        return NULL;
    }
    ps5vk_size(&w, &h, NULL);
    if (w <= 0 || h <= 0) {
        SDL_SetError("PS5VK: the display is not open");
        return NULL;
    }
    renderer = (SDL_Renderer *)SDL_calloc(1, sizeof(*renderer));
    if (!renderer) {
        SDL_OutOfMemory();
        return NULL;
    }
    renderer->window = window;

    renderer->WindowEvent = PS5VK_WindowEvent;
    renderer->GetOutputSize = PS5VK_GetOutputSize;
    renderer->SupportsBlendMode = PS5VK_SupportsBlendMode;
    renderer->CreateTexture = PS5VK_CreateTexture;
    renderer->UpdateTexture = PS5VK_UpdateTexture;
    renderer->LockTexture = PS5VK_LockTexture;
    renderer->UnlockTexture = PS5VK_UnlockTexture;
    renderer->SetTextureScaleMode = PS5VK_SetTextureScaleMode;
    renderer->SetRenderTarget = PS5VK_SetRenderTarget;
    renderer->QueueSetViewport = PS5VK_QueueNoop;
    renderer->QueueSetDrawColor = PS5VK_QueueNoop;
    renderer->QueueDrawPoints = PS5VK_QueueDrawPoints;
    renderer->QueueDrawLines = PS5VK_QueueDrawPoints;
    renderer->QueueGeometry = PS5VK_QueueGeometry;
    renderer->RunCommandQueue = PS5VK_RunCommandQueue;
    renderer->RenderReadPixels = PS5VK_RenderReadPixels;
    renderer->RenderPresent = PS5VK_RenderPresent;
    renderer->DestroyTexture = PS5VK_DestroyTexture;
    renderer->DestroyRenderer = PS5VK_DestroyRenderer;
    renderer->SetVSync = PS5VK_SetVSync;

    renderer->rect_index_order[0] = 0;
    renderer->rect_index_order[1] = 1;
    renderer->rect_index_order[2] = 3;
    renderer->rect_index_order[3] = 1;
    renderer->rect_index_order[4] = 3;
    renderer->rect_index_order[5] = 2;

    renderer->info = PS5VK_RenderDriver.info;
    renderer->info.flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    return renderer;
}

SDL_RenderDriver PS5VK_RenderDriver = {
    PS5VK_CreateRenderer,
    {
        "ps5vk",
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC,
        2,
        { SDL_PIXELFORMAT_ABGR8888, SDL_PIXELFORMAT_ARGB8888 },
        8192,
        8192,
    }
};

#endif /* SDL_VIDEO_RENDER_PS5VK */
