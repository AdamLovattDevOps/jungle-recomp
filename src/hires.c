/* hires.c — draw the frame with the GPU from the high-resolution art cache.
 *
 * The engine's draw list (engine_drawlist) gives each bitmap and text object
 * in painting order at canvas positions. Each bitmap is drawn from
 * DIR/<CONTAINER>/<NNNNN>.png when tools/upscale.py has made one (Real-ESRGAN,
 * 3x by default), otherwise from its 8-bit pixels through the scene palette,
 * so a partial cache still shows a whole picture. Textures are made on first
 * use and dropped when the scene changes. Fades scale every texture's colour,
 * as the palette fade scales the palette.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "engine.h"
#include "hires.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct { SDL_Texture *tex; u32 version; u8 tried, art; } Slot;

struct HiRes {
    SDL_Renderer *ren;
    char dir[512], scene[16];
    int level;                       /* art scale: 1, 2 (from x4, box-filtered), 3 (x3), 4 (x4) */
    Slot *bm, *txt; int n;
    int loaded, missing;
};

static int level_dir(const char *base, int level, char *out, size_t cap)
{
    snprintf(out, cap, "%s/x%d", base, level == 3 ? 3 : 4);
    SDL_RWops *rw = NULL;
    char probe[600]; snprintf(probe, sizeof probe, "%s/JUNGMAIN/.done-x%d", out, level == 3 ? 3 : 4);
    rw = SDL_RWFromFile(probe, "rb");
    if (rw) { SDL_RWclose(rw); return 1; }
    return 0;
}

int hires_has_level(const char *base, int level)
{
    char d[600];
    return (level == 1 || level == 2 || level == 3 || level == 4) && level_dir(base, level, d, sizeof d);
}

HiRes *hires_open(SDL_Renderer *ren, const char *base, int level)
{
    HiRes *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->ren = ren; h->level = level;
    if (!level_dir(base, level, h->dir, sizeof h->dir)) snprintf(h->dir, sizeof h->dir, "%s/x%d", base, level);
    return h;
}

static void drop(HiRes *h)
{
    for (int i = 0; i < h->n; i++) {
        if (h->bm[i].tex) SDL_DestroyTexture(h->bm[i].tex);
        if (h->txt[i].tex) SDL_DestroyTexture(h->txt[i].tex);
    }
    free(h->bm); free(h->txt); h->bm = h->txt = NULL; h->n = 0;
}

void hires_close(HiRes *h) { if (h) { drop(h); free(h); } }

/* 8-bit pixels through the palette, index 0 transparent. */
static SDL_Texture *from_indexed(HiRes *h, const u8 *px, int w, int ht, const u8 pal[256][3])
{
    u32 *rgba = malloc((size_t)w * ht * 4);
    if (!rgba) return NULL;
    for (int i = 0; i < w * ht; i++)
        rgba[i] = px[i] ? 0xFF000000u | ((u32)pal[px[i]][0] << 16) | ((u32)pal[px[i]][1] << 8) | pal[px[i]][2] : 0;
    SDL_Texture *t = SDL_CreateTexture(h->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, ht);
    if (t) {
        SDL_UpdateTexture(t, NULL, rgba, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    free(rgba);
    return t;
}

static SDL_Texture *from_png(HiRes *h, const char *path)
{
    int w, ht, n;
    u8 *px = stbi_load(path, &w, &ht, &n, 4);
    if (!px) return NULL;
    int f = h->level == 1 ? 4 : h->level == 2 ? 2 : 1;   /* x4 art box-filtered to 1x or 2x */
    if (f > 1 && w >= f && ht >= f) {
        int nw = w / f, nh = ht / f;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                unsigned acc[4] = { 0 }, a = 0;
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) {
                        const u8 *p = px + ((size_t)(y * f + j) * w + (x * f + i)) * 4;
                        acc[0] += p[0] * p[3]; acc[1] += p[1] * p[3]; acc[2] += p[2] * p[3]; a += p[3];
                    }
                u8 *o = px + ((size_t)y * nw + x) * 4;     /* in place: the write stays behind the reads */
                o[0] = (u8)(a ? acc[0] / a : 0); o[1] = (u8)(a ? acc[1] / a : 0); o[2] = (u8)(a ? acc[2] / a : 0);
                o[3] = (u8)(a / (unsigned)(f * f));
            }
        w = nw; ht = nh;
    }
    SDL_Texture *t = SDL_CreateTexture(h->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, ht);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    stbi_image_free(px);
    return t;
}

int hires_draw(HiRes *h, Engine *e, SDL_FRect dst)
{
    static EngDraw dl[4096];
    int nd = engine_drawlist(e, dl, 4096);
    if (nd == 1 && dl[0].kind == 3) return 0;          /* a fade-out shows the frozen 8-bit frame */
    if (strcmp(h->scene, e->name) || h->n != e->c.ndir) {
        drop(h);
        snprintf(h->scene, sizeof h->scene, "%s", e->name);
        h->n = e->c.ndir;
        h->bm = calloc((size_t)h->n, sizeof *h->bm);
        h->txt = calloc((size_t)h->n, sizeof *h->txt);
        if (!h->bm || !h->txt) { drop(h); return 0; }
    }
    char stem[16]; snprintf(stem, sizeof stem, "%s", e->name);
    char *dot = strchr(stem, '.'); if (dot) *dot = 0;
    float sc = dst.w / ENG_W;
    Uint8 mod = (Uint8)(e->fade.level * 255.0f + 0.5f);
    for (int k = 0; k < nd; k++) {
        EngDraw *d = &dl[k];
        if (d->kind == 0) {
            const u8 *c = e->c.pal[d->fill];
            SDL_SetRenderDrawColor(h->ren, (Uint8)(c[0] * mod / 255), (Uint8)(c[1] * mod / 255), (Uint8)(c[2] * mod / 255), 255);
            SDL_RenderFillRectF(h->ren, &dst);
            continue;
        }
        if (d->res < 0 || d->res >= h->n) continue;
        SDL_Texture *t = NULL;
        if (d->kind == 1) {
            Slot *s = &h->bm[d->res];
            if (!s->tried) {
                s->tried = 1;
                char path[700]; snprintf(path, sizeof path, "%s/%s/%05d.png", h->dir, stem, d->res);
                s->tex = from_png(h, path);
                if (s->tex) { s->art = 1; h->loaded++; }
                else { s->tex = from_indexed(h, d->px, d->w, d->h, e->c.pal); h->missing++; }
            }
            t = s->tex;
        } else {                                        /* text: rebuilt whenever it changes */
            Slot *s = &h->txt[d->res];
            if (!s->tex || s->version != d->version) {
                if (s->tex) SDL_DestroyTexture(s->tex);
                s->tex = from_indexed(h, d->px, d->w, d->h, e->c.pal);
                s->version = d->version;
            }
            t = s->tex;
        }
        if (!t) continue;
        SDL_SetTextureColorMod(t, mod, mod, mod);
        SDL_FRect r = { dst.x + d->x * sc, dst.y + d->y * sc, d->w * sc, d->h * sc };
        SDL_RenderCopyExF(h->ren, t, NULL, &r, 0, NULL, d->flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    }
    return 1;
}

void hires_stats(HiRes *h, int *art, int *fallback) { *art = h->loaded; *fallback = h->missing; }
