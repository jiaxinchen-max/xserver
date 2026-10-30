/*
 * Copyright © 2014 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "glamor_priv.h"
#include "glamor_transfer.h"

/* GLES has no X1555 upload type: GL_UNSIGNED_SHORT_5_5_5_1 puts every
 * channel at a different bit position. Convert pixel storage, never rendering. */
static void
glamor_upload_pixels(const struct glamor_format *f, int x, int y,
                     int width, int height, const uint8_t *bits, uint32_t stride)
{
    uint8_t *rgba;
    GLint row_length;
    int row, col;

    if (!f->rgba_upload) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height,
                       f->format, f->type, bits);
        return;
    }
    rgba = malloc((size_t) width * 4);
    if (!rgba)
        FatalError("Xlorie: cannot allocate X1555 transfer row\n");
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (row = 0; row < height; row++) {
        const uint8_t *src = bits + (size_t) row * stride;
        for (col = 0; col < width; col++) {
            unsigned pixel = src[col * 2] | ((unsigned) src[col * 2 + 1] << 8);
            rgba[col * 4] = (((pixel >> 10) & 31) * 255 + 15) / 31;
            rgba[col * 4 + 1] = (((pixel >> 5) & 31) * 255 + 15) / 31;
            rgba[col * 4 + 2] = ((pixel & 31) * 255 + 15) / 31;
            rgba[col * 4 + 3] = 255;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y + row, width, 1,
                       GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    free(rgba);
}

/*
 * Write a region of bits into a pixmap
 */
void
glamor_upload_boxes(PixmapPtr pixmap, BoxPtr in_boxes, int in_nbox,
                    int dx_src, int dy_src,
                    int dx_dst, int dy_dst,
                    uint8_t *bits, uint32_t byte_stride)
{
    ScreenPtr                   screen = pixmap->drawable.pScreen;
    glamor_screen_private       *glamor_priv = glamor_get_screen_private(screen);
    glamor_pixmap_private       *priv = glamor_get_pixmap_private(pixmap);
    int                         box_index;
    int                         bytes_per_pixel = pixmap->drawable.bitsPerPixel >> 3;
    const struct glamor_format *f = glamor_format_for_pixmap(pixmap);

    glamor_make_current(glamor_priv);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (glamor_priv->has_unpack_subimage)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, byte_stride / bytes_per_pixel);

    glamor_pixmap_loop(priv, box_index) {
        BoxPtr                  box = glamor_pixmap_box_at(priv, box_index);
        glamor_pixmap_fbo       *fbo = glamor_pixmap_fbo_at(priv, box_index);
        BoxPtr                  boxes = in_boxes;
        int                     nbox = in_nbox;

        glamor_bind_texture(glamor_priv, GL_TEXTURE0, fbo, TRUE);

        while (nbox--) {

            /* compute drawable coordinates */
            int x1 = MAX(boxes->x1 + dx_dst, box->x1);
            int x2 = MIN(boxes->x2 + dx_dst, box->x2);
            int y1 = MAX(boxes->y1 + dy_dst, box->y1);
            int y2 = MIN(boxes->y2 + dy_dst, box->y2);

            size_t ofs = (y1 - dy_dst + dy_src) * byte_stride;
            ofs += (x1 - dx_dst + dx_src) * bytes_per_pixel;

            boxes++;

            if (x2 <= x1 || y2 <= y1)
                continue;

            if (glamor_priv->has_unpack_subimage ||
                x2 - x1 == byte_stride / bytes_per_pixel) {
                glamor_upload_pixels(f, x1 - box->x1, y1 - box->y1,
                                     x2 - x1, y2 - y1, bits + ofs, byte_stride);
            } else {
                for (; y1 < y2; y1++, ofs += byte_stride)
                    glamor_upload_pixels(f, x1 - box->x1, y1 - box->y1,
                                         x2 - x1, 1, bits + ofs, byte_stride);
            }
        }
    }

    if (glamor_priv->has_unpack_subimage)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/*
 * Upload a region of data
 */

void
glamor_upload_region(PixmapPtr pixmap, RegionPtr region,
                     int region_x, int region_y,
                     uint8_t *bits, uint32_t byte_stride)
{
    glamor_upload_boxes(pixmap, RegionRects(region), RegionNumRects(region),
                        -region_x, -region_y,
                        0, 0,
                        bits, byte_stride);
}

/*
 * Take the data in the pixmap and stuff it back into the FBO
 */
void
glamor_upload_pixmap(PixmapPtr pixmap)
{
    BoxRec box;

    box.x1 = 0;
    box.x2 = pixmap->drawable.width;
    box.y1 = 0;
    box.y2 = pixmap->drawable.height;
    glamor_upload_boxes(pixmap, &box, 1, 0, 0, 0, 0,
                        pixmap->devPrivate.ptr, pixmap->devKind);
}

/* GLES implementations may support BGRA uploads but only RGBA readback.
 * Convert only the requested rectangle: neighbouring pixels and row padding
 * can belong to other download boxes and must remain untouched.
 */
static void
glamor_read_pixels(const struct glamor_format *f, int x, int y, int width,
                   int height, uint8_t *bits, uint32_t stride)
{
    if (f->rgba_readback && (f->depth == 8 || f->depth == 15 || f->depth == 16)) {
        uint8_t *rgba = malloc((size_t) width * 4);
        GLint row_length;
        int row, col;

        if (!rgba)
            FatalError("Xlorie: cannot allocate exact RGBA transfer row\n");
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        for (row = 0; row < height; row++) {
            uint8_t *dst = bits + (size_t) row * stride;
            glReadPixels(x, y + row, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            for (col = 0; col < width; col++) {
                const uint8_t *pixel = rgba + col * 4;
                if (f->depth == 8) {
                    dst[col] = pixel[0];
                } else {
                    unsigned red = (pixel[0] * 31 + 127) / 255;
                    unsigned blue = (pixel[2] * 31 + 127) / 255;
                    unsigned green = (pixel[1] * (f->depth == 16 ? 63 : 31) + 127) / 255;
                    unsigned value = f->depth == 16 ? (red << 11) | (green << 5) | blue
                                                    : (red << 10) | (green << 5) | blue;
                    dst[col * 2] = value & 255;
                    dst[col * 2 + 1] = value >> 8;
                }
            }
        }
        glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
        free(rgba);
        return;
    }
    glReadPixels(x, y, width, height,
                 f->rgba_readback ? GL_RGBA : f->format, f->type, bits);
    if (f->rgba_readback) {
        for (int row = 0; row < height; row++) {
            uint8_t *pixel = bits + (size_t) row * stride;
            for (int col = 0; col < width; col++, pixel += 4) {
                uint8_t red = pixel[0];
                pixel[0] = pixel[2];
                pixel[2] = red;
            }
        }
    }
}

/*
 * Read stuff from the pixmap FBOs and write to memory
 */
void
glamor_download_boxes(PixmapPtr pixmap, BoxPtr in_boxes, int in_nbox,
                      int dx_src, int dy_src,
                      int dx_dst, int dy_dst,
                      uint8_t *bits, uint32_t byte_stride)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    glamor_pixmap_private *priv = glamor_get_pixmap_private(pixmap);
    int box_index;
    int bytes_per_pixel = pixmap->drawable.bitsPerPixel >> 3;
    const struct glamor_format *f = glamor_format_for_pixmap(pixmap);

    glamor_make_current(glamor_priv);

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (glamor_priv->has_pack_subimage)
        glPixelStorei(GL_PACK_ROW_LENGTH, byte_stride / bytes_per_pixel);

    glamor_pixmap_loop(priv, box_index) {
        BoxPtr                  box = glamor_pixmap_box_at(priv, box_index);
        glamor_pixmap_fbo       *fbo = glamor_pixmap_fbo_at(priv, box_index);
        BoxPtr                  boxes = in_boxes;
        int                     nbox = in_nbox;

        /* This should not be called on GLAMOR_FBO_NO_FBO-allocated pixmaps. */
        assert(fbo->fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo->fb);

        while (nbox--) {

            /* compute drawable coordinates */
            int                     x1 = MAX(boxes->x1 + dx_src, box->x1);
            int                     x2 = MIN(boxes->x2 + dx_src, box->x2);
            int                     y1 = MAX(boxes->y1 + dy_src, box->y1);
            int                     y2 = MIN(boxes->y2 + dy_src, box->y2);
            size_t ofs = (y1 - dy_src + dy_dst) * byte_stride;
            ofs += (x1 - dx_src + dx_dst) * bytes_per_pixel;

            boxes++;

            if (x2 <= x1 || y2 <= y1)
                continue;

            if (glamor_priv->has_pack_subimage ||
                x2 - x1 == byte_stride / bytes_per_pixel) {
                glamor_read_pixels(f, x1 - box->x1, y1 - box->y1, x2 - x1, y2 - y1, bits + ofs, byte_stride);
            } else {
                for (; y1 < y2; y1++, ofs += byte_stride)
                    glamor_read_pixels(f, x1 - box->x1, y1 - box->y1, x2 - x1, 1, bits + ofs, byte_stride);
            }
        }
    }
    if (glamor_priv->has_pack_subimage)
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

/*
 * Read data from the pixmap FBO
 */
void
glamor_download_rect(PixmapPtr pixmap, int x, int y, int w, int h, uint8_t *bits)
{
    BoxRec      box;

    box.x1 = x;
    box.x2 = x + w;
    box.y1 = y;
    box.y2 = y + h;

    glamor_download_boxes(pixmap, &box, 1, 0, 0, -x, -y,
                          bits, PixmapBytePad(w, pixmap->drawable.depth));
}

/*
 * Pull the data from the FBO down to the pixmap
 */
void
glamor_download_pixmap(PixmapPtr pixmap)
{
    BoxRec      box;

    box.x1 = 0;
    box.x2 = pixmap->drawable.width;
    box.y1 = 0;
    box.y2 = pixmap->drawable.height;

    glamor_download_boxes(pixmap, &box, 1, 0, 0, 0, 0,
                          pixmap->devPrivate.ptr, pixmap->devKind);
}
