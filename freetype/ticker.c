/* Simple scrolling ticker
 * Freetype portion heavily derived from the example code in the the Freetype
 * tutorial.
 * Freetype usage is basic - could easily be improved to have things like
 * different colour outlines, rendering in RGB rather than grey etc.
 */

#include "ticker.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <wayout.h>

#include <drm_fourcc.h>

enum ticker_state_e {
    TICKER_NEW = 0,
    TICKER_NEXT_CHAR,
    TICKER_SCROLL
};

struct ticker_env_s {
    enum ticker_state_e state;

    wo_window_t *wowin;
    wo_surface_t *dp;
    wo_fb_t *dfbs[2];

    uint32_t format;
    uint64_t modifier;

    wo_rect_t pos;      // Scaled pos
    wo_rect_t base_pos;
    wo_rect_t win_pos;  // Window size that base_pos was placed in

    FT_Library    library;
    FT_Face       face;

    FT_Vector     pen;                    /* untransformed origin  */
    FT_Bool use_kerning;
    FT_UInt previous;

    unsigned int bn;  // Buffer for render
    int shl;          // Scroll left amount (-ve => need a new char)
    int shl_per_run;  // Amount to scroll per run

    int           target_height;
    int           target_width;
    unsigned int bb_width;

    ticker_next_char_fn next_char_cb;
    void *next_char_v;
};

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

static inline uint32_t
grey2argb(const uint32_t x)
{
    return ((x << 24) | (x << 16) | (x << 8) | (x));
}

void
draw_bitmap(wo_fb_t *const dfb,
            FT_Bitmap *bitmap,
            FT_Int      x,
            FT_Int      y)
{
    int  i, j, p, q;
    const int fb_width = wo_fb_width(dfb);
    const int fb_height = wo_fb_height(dfb);
    const size_t fb_stride = wo_fb_pitch(dfb, 0) / 4;
    uint32_t *const image = wo_fb_data(dfb, 0);

    const int  x_max = MIN(fb_width, (int)(x + bitmap->width));
    const int  y_max = MIN(fb_height, (int)(y + bitmap->rows));

    /* for simplicity, we assume that `bitmap->pixel_mode' */
    /* is `FT_PIXEL_MODE_GRAY' (i.e., not a bitmap font)   */

    for (j = y, q = 0; j < y_max; j++, q++)
    {
        for (i = x, p = 0; i < x_max; i++, p++)
            image[j * fb_stride + i] |= grey2argb(bitmap->buffer[q * bitmap->width + p]);
    }
}

static void
shift_2d(void *dst, const void *src, size_t stride, size_t offset, size_t h)
{
    size_t i;
    uint8_t *d = dst;
    const uint8_t *s = src;

    for (i = 0; i != h; ++i)
    {
        memcpy(d, s + offset, stride - offset);
        memset(d + stride - offset, 0, offset);
        d += stride;
        s += stride;
    }
}

void
ticker_next_char_cb_set(ticker_env_t *const te, const ticker_next_char_fn fn, void *const v)
{
    te->next_char_cb = fn;
    te->next_char_v = v;
}

void
ticker_commit_cb_set(ticker_env_t *const te, void (* commit_cb)(void * v), void * commit_v)
{
    (void)te;
    (void)commit_cb;
    (void)commit_v;
}

static int
do_scroll(ticker_env_t *const te)
{
    if (te->shl < 0)
    {
        te->state = TICKER_NEXT_CHAR;
        return 1;
    }
    else
    {
        wo_fb_t *const fb0 = te->dfbs[te->bn];

//        printf("tw=%d, pos.w=%d, shl=%d, x=%d\n",
//               te->target_width, (int)te->base_pos.w, te->shl,
//               te->target_width - (int)te->base_pos.w - te->shl);
        wo_fb_crop_frac_set(fb0, (wo_rect_t) {.x = MAX(0, te->target_width - (int)te->base_pos.w - te->shl) << 16, .y = 0,
                                              .w = te->base_pos.w << 16, .h = te->base_pos.h << 16 });
        wo_surface_attach_fb(te->dp, fb0, te->pos);
        wo_surface_commit(te->dp);

        te->shl -= te->shl_per_run;
        return 0;
    }
}

static int
do_render(ticker_env_t *const te)
{
    FT_Matrix matrix = {
        .xx = 0x10000L,
        .xy = 0,
        .yx = 0,
        .yy = 0x10000L
    };
    const FT_GlyphSlot slot = te->face->glyph;
    FT_UInt glyph_index;
    int c;
    wo_fb_t *const fb1 = te->dfbs[te->bn];
    wo_fb_t *const fb0 = te->dfbs[te->bn ^ 1];
    int shl1;

    /* set transformation */
    FT_Set_Transform(te->face, &matrix, &te->pen);

    c = te->next_char_cb(te->next_char_v);
    if (c <= 0)
    {
        // If the window didn't quite get to end end of the buffer on last
        // scroll then set it there.
        if (te->shl + te->shl_per_run > 0)
        {
            te->shl = 0;
            do_scroll(te);
        }
        return c;
    }

    /* convert character code to glyph index */
    glyph_index = FT_Get_Char_Index(te->face, c);

    /* retrieve kerning distance and move pen position */
    if (te->use_kerning && te->previous && glyph_index)
    {
        FT_Vector delta = { 0, 0 };
        FT_Get_Kerning(te->face, te->previous, glyph_index, FT_KERNING_DEFAULT, &delta);
        te->pen.x += delta.x;
    }

    /* load glyph image into the slot (erase previous one) */
    if (FT_Load_Glyph(te->face, glyph_index, FT_LOAD_RENDER))
    {
        fprintf(stderr, "Load Glyph failed");
        return -1;
    }

    wo_fb_write_start(fb0);
    shl1 = MAX(slot->bitmap_left + slot->bitmap.width, (te->pen.x + slot->advance.x) >> 6) - te->target_width;
    if (shl1 > 0)
    {
        te->pen.x -= shl1 << 6;
        shift_2d(wo_fb_data(fb0, 0), wo_fb_data(fb1, 0), wo_fb_pitch(fb0, 0), shl1 * 4, wo_fb_height(fb0));
    }

    // now, draw to our target surface (convert position)
    draw_bitmap(fb0, &slot->bitmap, slot->bitmap_left - shl1, te->target_height - slot->bitmap_top);
    wo_fb_write_end(fb0);

    /* increment pen position */
    te->pen.x += slot->advance.x;
    te->shl += shl1;

    te->previous = glyph_index;
    te->bn ^= 1;
    te->state = TICKER_SCROLL;
    return 1;
}

int
ticker_run(ticker_env_t *const te)
{
    int rv = -1;
    do
    {
        switch (te->state)
        {
            case TICKER_NEW:
            case TICKER_NEXT_CHAR:
                rv = do_render(te);
                break;
            case TICKER_SCROLL:
                rv = do_scroll(te);
                break;
            default:
                break;
        }
    } while (rv == 1);
    return rv;
}

void
ticker_delete(ticker_env_t **ppTicker)
{
    ticker_env_t *const te = *ppTicker;
    if (te == NULL)
        return;

    if (te->dfbs[0])
    {
        wo_surface_detach_fb(te->dp);
        wo_surface_commit(te->dp);
    }

    wo_fb_unref(te->dfbs + 0);
    wo_fb_unref(te->dfbs + 1);
    wo_surface_unref(&te->dp);

    FT_Done_Face(te->face);
    FT_Done_FreeType(te->library);

    free(te);
}

int
ticker_init(ticker_env_t *const te)
{
    wo_env_t * const woe = wo_window_env(te->wowin);
    for (unsigned int i = 0; i != 2; ++i)
    {
        te->dfbs[i] = wo_make_fb(woe, te->target_width, te->base_pos.h, te->format, te->modifier);
        if (te->dfbs[i] == NULL)
        {
            fprintf(stderr, "Failed to get frame buffer");
            return -1;
        }
    }

    wo_fb_write_start(te->dfbs[0]);
    memset(wo_fb_data(te->dfbs[0], 0), 0x00, wo_fb_height(te->dfbs[0]) * wo_fb_pitch(te->dfbs[0], 0));
    wo_fb_write_end(te->dfbs[0]);
    return 0;
}

int
ticker_set_face(ticker_env_t *const te, const char *const filename)
{
    const FT_Pos buf_height = te->base_pos.h - 2; // Allow 1 pixel T&B for rounding
    FT_Pos scaled_size;
    FT_Pos bb_height;

// https://freetype.org/freetype2/docs/tutorial/step2.html

    if (FT_New_Face(te->library, filename, 0, &te->face))
    {
        fprintf(stderr, "Face not found '%s'", filename);
        return -1;
    }

    bb_height = te->face->bbox.yMax - te->face->bbox.yMin;
    te->bb_width = FT_MulDiv(te->face->bbox.xMax - te->face->bbox.xMin, buf_height, bb_height);
    scaled_size = FT_MulDiv(te->face->units_per_EM, buf_height, bb_height);

//    printf("UPer Em=%d, scaled=%ld, height=%ld\n", te->face->units_per_EM, scaled_size, buf_height);
//    printf("BBox=%ld,%ld->%ld,%ld =%ld, bb_scaled_w=%d\n", te->face->bbox.xMin, te->face->bbox.yMin, te->face->bbox.xMax, te->face->bbox.yMax, bb_height, te->bb_width);

    if (FT_Set_Pixel_Sizes(te->face, 0, scaled_size))
    {
        fprintf(stderr, "Bad char size\n");
        return -1;
    }

    te->pen.y =  FT_MulDiv(-te->face->bbox.yMin * 32, buf_height, bb_height) + 32;
    te->target_height = (int)((FT_Pos)te->base_pos.h - (te->pen.y >> 6)); // Top for rendering purposes
    te->target_width = MAX(te->bb_width, te->base_pos.w) + te->bb_width;
    te->pen.x = te->target_width * 64; // Start with X pos @ far right hand side

    te->use_kerning = FT_HAS_KERNING(te->face);
    return 0;
}

int
ticker_set_shl(ticker_env_t *const te, unsigned int shift_pels)
{
    te->shl_per_run = shift_pels;
    return 0;
}

// This isn't really the way to do this
// We should probably have a complete rebuild based on the new size
// but that isn't supported yet (and is very processor intensive if
// part of a dynamic resize)
static void
win_resize_cb(void * v, wo_surface_t * wos, const wo_rect_t win_pos)
{
    ticker_env_t * const te = v;

    te->pos = wo_rect_rescale(te->base_pos, win_pos, te->win_pos);
    wo_surface_dst_pos_set(wos, te->pos);
}

ticker_env_t*
ticker_new(struct wo_window_s * wowin, const wo_rect_t pos, const wo_rect_t win_pos)
{
    ticker_env_t *te = calloc(1, sizeof(*te));

    if (te == NULL)
        return NULL;

    te->wowin = wowin;

    te->pos = pos;
    te->base_pos = pos;
    te->win_pos = win_pos;
    te->format = DRM_FORMAT_ARGB8888;
    te->modifier = DRM_FORMAT_MOD_LINEAR;
    te->shl_per_run = 3;

    if (FT_Init_FreeType(&te->library) != 0)
    {
        fprintf(stderr, "Failed to init FreeType");
        goto fail;
    }

    // This doesn't really want to be the primary
    if ((te->dp = wo_make_surface_z(te->wowin, NULL, 16)) == NULL)
    {
        fprintf(stderr, "Failed to find output plane");
        goto fail;
    }

    wo_surface_on_win_resize_set(te->dp, win_resize_cb, te);

    return te;

fail:
    ticker_delete(&te);
    return NULL;
}

