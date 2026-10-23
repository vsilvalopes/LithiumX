// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2022 Ryzee119

#include "lithiumx.h"
#include "dash_xbe_icon.h"
#include <stdio.h>
#include <stdlib.h>

#if LV_COLOR_DEPTH != 32
#error "dash_xbe_icon.c outputs 32bit LV_IMG_CF_TRUE_COLOR_ALPHA images"
#endif

#define XBE_MAX_HEADER_SIZE (64 * 1024)
#define XBE_MAX_IMAGE_SIZE (512 * 1024)
#define XBE_MAX_IMAGE_DIM 512

typedef struct __attribute((packed))
{
    uint32_t dwFlags;
    uint32_t dwVirtualAddr;
    uint32_t dwVirtualSize;
    uint32_t dwRawAddr;
    uint32_t dwSizeofRaw;
    uint32_t dwSectionNameAddr;
    uint32_t dwSectionRefCount;
    uint32_t dwHeadSharedRefCountAddr;
    uint32_t dwTailSharedRefCountAddr;
    uint8_t bzSectionDigest[20];
} xbe_section_header_t;

// Xbox D3D texture formats (NV2A) that may be found in a title image
enum
{
    XPR_FMT_A1R5G5B5 = 0x02,
    XPR_FMT_X1R5G5B5 = 0x03,
    XPR_FMT_A4R4G4B4 = 0x04,
    XPR_FMT_R5G6B5 = 0x05,
    XPR_FMT_A8R8G8B8 = 0x06,
    XPR_FMT_X8R8G8B8 = 0x07,
    XPR_FMT_DXT1 = 0x0C,
    XPR_FMT_DXT3 = 0x0E,
    XPR_FMT_DXT5 = 0x0F,
    XPR_FMT_LIN_A1R5G5B5 = 0x10,
    XPR_FMT_LIN_R5G6B5 = 0x11,
    XPR_FMT_LIN_A8R8G8B8 = 0x12,
    XPR_FMT_LIN_X1R5G5B5 = 0x1C,
    XPR_FMT_LIN_A4R4G4B4 = 0x1D,
    XPR_FMT_LIN_X8R8G8B8 = 0x1E,
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t from_565(uint16_t c)
{
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return argb(0xFF, (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

static uint32_t from_1555(uint16_t c, bool has_alpha)
{
    uint32_t r = (c >> 10) & 0x1F, g = (c >> 5) & 0x1F, b = c & 0x1F;
    uint32_t a = (has_alpha == false || (c & 0x8000)) ? 0xFF : 0x00;
    return argb(a, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
}

static uint32_t from_4444(uint16_t c)
{
    return argb(((c >> 12) & 0xF) * 0x11, ((c >> 8) & 0xF) * 0x11, ((c >> 4) & 0xF) * 0x11, (c & 0xF) * 0x11);
}

// Offset of texel (x,y) in a swizzled (morton order) texture of size w x h. w and h are powers of 2
static uint32_t swizzle_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t offset = 0;
    int shift = 0;
    for (uint32_t mask = 1; mask < w || mask < h; mask <<= 1)
    {
        if (mask < w)
        {
            offset |= ((x & mask) ? 1U : 0U) << shift++;
        }
        if (mask < h)
        {
            offset |= ((y & mask) ? 1U : 0U) << shift++;
        }
    }
    return offset;
}

// Decode the 4x4 colour part of a DXT block into out (16 ARGB pixels)
static void dxt_color_block(const uint8_t *block, uint32_t *out, bool allow_1bit_alpha)
{
    uint16_t c0 = rd16(&block[0]);
    uint16_t c1 = rd16(&block[2]);
    uint32_t p0 = from_565(c0), p1 = from_565(c1);
    uint32_t pal[4];
    pal[0] = p0;
    pal[1] = p1;

    uint32_t r0 = (p0 >> 16) & 0xFF, g0 = (p0 >> 8) & 0xFF, b0 = p0 & 0xFF;
    uint32_t r1 = (p1 >> 16) & 0xFF, g1 = (p1 >> 8) & 0xFF, b1 = p1 & 0xFF;
    if (c0 > c1 || allow_1bit_alpha == false)
    {
        pal[2] = argb(0xFF, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
        pal[3] = argb(0xFF, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
    }
    else
    {
        pal[2] = argb(0xFF, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
        pal[3] = 0x00000000;
    }

    uint32_t indices = rd32(&block[4]);
    for (int i = 0; i < 16; i++)
    {
        out[i] = pal[(indices >> (2 * i)) & 0x3];
    }
}

static void dxt_decode(const uint8_t *src, uint32_t *dst, int w, int h, int fmt)
{
    int block_size = (fmt == XPR_FMT_DXT1) ? 8 : 16;
    uint32_t px[16];
    for (int by = 0; by < (h + 3) / 4; by++)
    {
        for (int bx = 0; bx < (w + 3) / 4; bx++)
        {
            const uint8_t *block = src;
            src += block_size;

            if (fmt == XPR_FMT_DXT1)
            {
                dxt_color_block(block, px, true);
            }
            else
            {
                dxt_color_block(&block[8], px, false);
                uint8_t alpha[16];
                if (fmt == XPR_FMT_DXT3)
                {
                    for (int i = 0; i < 16; i++)
                    {
                        alpha[i] = ((block[i / 2] >> ((i & 1) * 4)) & 0xF) * 0x11;
                    }
                }
                else
                {
                    uint32_t a0 = block[0], a1 = block[1];
                    uint8_t apal[8] = {(uint8_t)a0, (uint8_t)a1};
                    for (int i = 2; i < 8; i++)
                    {
                        if (a0 > a1)
                        {
                            apal[i] = (uint8_t)(((8 - i) * a0 + (i - 1) * a1) / 7);
                        }
                        else
                        {
                            apal[i] = (i < 6) ? (uint8_t)(((6 - i) * a0 + (i - 1) * a1) / 5) : ((i == 6) ? 0 : 255);
                        }
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++)
                    {
                        bits |= (uint64_t)block[2 + i] << (8 * i);
                    }
                    for (int i = 0; i < 16; i++)
                    {
                        alpha[i] = apal[(bits >> (3 * i)) & 0x7];
                    }
                }
                for (int i = 0; i < 16; i++)
                {
                    px[i] = (px[i] & 0x00FFFFFF) | ((uint32_t)alpha[i] << 24);
                }
            }

            for (int i = 0; i < 16; i++)
            {
                int x = bx * 4 + (i % 4);
                int y = by * 4 + (i / 4);
                if (x < w && y < h)
                {
                    dst[y * w + x] = px[i];
                }
            }
        }
    }
}

// Decodes a texture into ARGB pixels. Returns false if the format is not supported or the data is too short.
static bool texture_decode(const uint8_t *src, uint32_t src_len, uint32_t *dst, int w, int h, int fmt, uint32_t pitch)
{
    if (fmt == XPR_FMT_DXT1 || fmt == XPR_FMT_DXT3 || fmt == XPR_FMT_DXT5)
    {
        uint32_t need = ((w + 3) / 4) * ((h + 3) / 4) * ((fmt == XPR_FMT_DXT1) ? 8 : 16);
        if (src_len < need)
        {
            return false;
        }
        dxt_decode(src, dst, w, h, fmt);
        return true;
    }

    int bpp;
    bool swizzled;
    switch (fmt)
    {
    case XPR_FMT_A1R5G5B5: case XPR_FMT_X1R5G5B5: case XPR_FMT_A4R4G4B4: case XPR_FMT_R5G6B5:
        bpp = 2; swizzled = true; break;
    case XPR_FMT_A8R8G8B8: case XPR_FMT_X8R8G8B8:
        bpp = 4; swizzled = true; break;
    case XPR_FMT_LIN_A1R5G5B5: case XPR_FMT_LIN_R5G6B5: case XPR_FMT_LIN_X1R5G5B5: case XPR_FMT_LIN_A4R4G4B4:
        bpp = 2; swizzled = false; break;
    case XPR_FMT_LIN_A8R8G8B8: case XPR_FMT_LIN_X8R8G8B8:
        bpp = 4; swizzled = false; break;
    default:
        return false;
    }

    if (pitch == 0)
    {
        pitch = w * bpp;
    }
    if (src_len < (swizzled ? (uint32_t)(w * h * bpp) : pitch * h))
    {
        return false;
    }

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            const uint8_t *p = (swizzled) ? &src[swizzle_offset(x, y, w, h) * bpp] : &src[y * pitch + x * bpp];
            uint32_t c;
            switch (fmt)
            {
            case XPR_FMT_A1R5G5B5: case XPR_FMT_LIN_A1R5G5B5: c = from_1555(rd16(p), true); break;
            case XPR_FMT_X1R5G5B5: case XPR_FMT_LIN_X1R5G5B5: c = from_1555(rd16(p), false); break;
            case XPR_FMT_A4R4G4B4: case XPR_FMT_LIN_A4R4G4B4: c = from_4444(rd16(p)); break;
            case XPR_FMT_R5G6B5: case XPR_FMT_LIN_R5G6B5: c = from_565(rd16(p)); break;
            case XPR_FMT_X8R8G8B8: case XPR_FMT_LIN_X8R8G8B8: c = rd32(p) | 0xFF000000; break;
            default: c = rd32(p); break;
            }
            dst[y * w + x] = c;
        }
    }
    return true;
}

// Decodes an uncompressed 24 or 32bit BMP into ARGB pixels. Attach XBEs made by Repackinator (Cerbios attacher)
// store the game's title image this way. Returns the pixels (malloc) or NULL.
static uint32_t *bmp_decode(const uint8_t *img, uint32_t len, int *w_out, int *h_out)
{
    if (len < 54)
    {
        return NULL;
    }
    uint32_t data_offset = rd32(&img[10]);
    int w = (int)rd32(&img[18]);
    int h = (int)rd32(&img[22]);
    int bpp = rd16(&img[28]);
    uint32_t compression = rd32(&img[30]);

    // Positive height is stored bottom up
    bool bottom_up = h > 0;
    h = (h < 0) ? -h : h;
    if (w <= 0 || h <= 0 || w > XBE_MAX_IMAGE_DIM || h > XBE_MAX_IMAGE_DIM ||
        (bpp != 24 && bpp != 32) || (compression != 0 && compression != 3))
    {
        return NULL;
    }

    uint32_t stride = ((w * bpp / 8) + 3) & ~3U;
    if (data_offset >= len || len - data_offset < stride * h)
    {
        return NULL;
    }

    uint32_t *pixels = malloc(w * h * sizeof(uint32_t));
    if (pixels == NULL)
    {
        return NULL;
    }

    // 32bit BMPs often leave the alpha byte as 0. Only use it if some pixel actually has alpha.
    bool use_alpha = false;
    for (int y = 0; y < h && bpp == 32 && use_alpha == false; y++)
    {
        const uint8_t *row = &img[data_offset + y * stride];
        for (int x = 0; x < w; x++)
        {
            if (row[x * 4 + 3] != 0)
            {
                use_alpha = true;
                break;
            }
        }
    }

    for (int y = 0; y < h; y++)
    {
        const uint8_t *row = &img[data_offset + (bottom_up ? (h - 1 - y) : y) * stride];
        for (int x = 0; x < w; x++)
        {
            const uint8_t *p = &row[x * (bpp / 8)];
            uint32_t a = (use_alpha) ? p[3] : 0xFF;
            pixels[y * w + x] = argb(a, p[2], p[1], p[0]);
        }
    }
    *w_out = w;
    *h_out = h;
    return pixels;
}

// Decodes an XPR0, DDS or BMP title image into ARGB pixels. Returns the pixels (malloc) or NULL.
static uint32_t *image_decode(const uint8_t *img, uint32_t len, int *w_out, int *h_out)
{
    int w, h, fmt;
    uint32_t pitch = 0;
    const uint8_t *data;
    uint32_t data_len;

    if (len >= 2 && memcmp(img, "BM", 2) == 0)
    {
        return bmp_decode(img, len, w_out, h_out);
    }
    else if (len >= 32 && memcmp(img, "XPR0", 4) == 0)
    {
        uint32_t header_size = rd32(&img[8]);
        uint32_t data_offset = rd32(&img[16]);
        uint32_t format = rd32(&img[24]);
        uint32_t size = rd32(&img[28]);

        fmt = (format >> 8) & 0xFF;
        if (size != 0)
        {
            // Linear textures store the dimensions in the size field
            w = (size & 0xFFF) + 1;
            h = ((size >> 12) & 0xFFF) + 1;
            pitch = ((size >> 24) + 1) * 64;
        }
        else
        {
            w = 1 << ((format >> 20) & 0xF);
            h = 1 << ((format >> 24) & 0xF);
        }

        uint32_t offset = header_size + data_offset;
        if (offset >= len)
        {
            offset = header_size;
        }
        if (offset >= len)
        {
            return NULL;
        }
        data = &img[offset];
        data_len = len - offset;
    }
    else if (len >= 128 && memcmp(img, "DDS ", 4) == 0)
    {
        h = (int)rd32(&img[12]);
        w = (int)rd32(&img[16]);
        if (memcmp(&img[84], "DXT1", 4) == 0)
        {
            fmt = XPR_FMT_DXT1;
        }
        else if (memcmp(&img[84], "DXT3", 4) == 0 || memcmp(&img[84], "DXT2", 4) == 0)
        {
            fmt = XPR_FMT_DXT3;
        }
        else if (memcmp(&img[84], "DXT5", 4) == 0 || memcmp(&img[84], "DXT4", 4) == 0)
        {
            fmt = XPR_FMT_DXT5;
        }
        else
        {
            return NULL;
        }
        data = &img[128];
        data_len = len - 128;
    }
    else
    {
        return NULL;
    }

    if (w <= 0 || h <= 0 || w > XBE_MAX_IMAGE_DIM || h > XBE_MAX_IMAGE_DIM)
    {
        return NULL;
    }

    uint32_t *pixels = malloc(w * h * sizeof(uint32_t));
    if (pixels == NULL)
    {
        return NULL;
    }
    if (texture_decode(data, data_len, pixels, w, h, fmt, pitch) == false)
    {
        free(pixels);
        return NULL;
    }
    *w_out = w;
    *h_out = h;
    return pixels;
}

// Reads the raw contents of the first section found in names[]. Returns the data (malloc) or NULL.
static uint8_t *xbe_read_image_section(FILE *fp, uint32_t *len_out)
{
    static const char *names[] = {"$$XTIMAGE", "$$XSIMAGE"};
    xbe_header_t xbe_header;
    uint8_t *headers = NULL;
    uint8_t *section_data = NULL;

    if (fread(&xbe_header, 1, sizeof(xbe_header), fp) != sizeof(xbe_header) ||
        memcmp(&xbe_header.dwMagic, "XBEH", 4) != 0)
    {
        return NULL;
    }

    // The section headers and their names are all inside the image header that is loaded at dwBaseAddr
    uint32_t header_len = LV_MIN(xbe_header.dwSizeofHeaders, XBE_MAX_HEADER_SIZE);
    headers = malloc(header_len);
    if (headers == NULL || fseek(fp, 0, SEEK_SET) != 0 || fread(headers, 1, header_len, fp) != header_len)
    {
        free(headers);
        return NULL;
    }

    uint32_t sh_offset = xbe_header.dwSectionHeadersAddr - xbe_header.dwBaseAddr;
    for (unsigned int n = 0; n < DASH_ARRAY_SIZE(names) && section_data == NULL; n++)
    {
        for (uint32_t i = 0; i < xbe_header.dwSections; i++)
        {
            uint32_t offset = sh_offset + i * sizeof(xbe_section_header_t);
            if (offset + sizeof(xbe_section_header_t) > header_len)
            {
                break;
            }
            xbe_section_header_t sh;
            memcpy(&sh, &headers[offset], sizeof(sh));

            uint32_t name_offset = sh.dwSectionNameAddr - xbe_header.dwBaseAddr;
            size_t name_len = strlen(names[n]);
            if (name_offset + name_len + 1 > header_len ||
                memcmp(&headers[name_offset], names[n], name_len + 1) != 0)
            {
                continue;
            }

            if (sh.dwSizeofRaw == 0 || sh.dwSizeofRaw > XBE_MAX_IMAGE_SIZE)
            {
                break;
            }
            section_data = malloc(sh.dwSizeofRaw);
            if (section_data == NULL || fseek(fp, sh.dwRawAddr, SEEK_SET) != 0 ||
                fread(section_data, 1, sh.dwSizeofRaw, fp) != sh.dwSizeofRaw)
            {
                free(section_data);
                section_data = NULL;
                break;
            }
            *len_out = sh.dwSizeofRaw;
            break;
        }
    }
    free(headers);
    return section_data;
}

lv_img_dsc_t *dash_xbe_icon_load(const char *xbe_path, int size)
{
    FILE *fp = fopen(xbe_path, "rb");
    if (fp == NULL)
    {
        return NULL;
    }
    uint32_t section_len = 0;
    uint8_t *section = xbe_read_image_section(fp, &section_len);
    fclose(fp);
    if (section == NULL)
    {
        return NULL;
    }

    int w, h;
    uint32_t *pixels = image_decode(section, section_len, &w, &h);
    free(section);
    if (pixels == NULL)
    {
        return NULL;
    }

    // The image has one extra fully transparent row at the top. The xgu driver caches textures by a
    // checksum of the first and middle pixels, which can be identical for different icons (transparent
    // edges). Storing a unique id in the invisible row keeps each icon's cache key unique.
    static uint32_t icon_id;
    uint32_t data_size = size * (size + 1) * sizeof(uint32_t);
    lv_img_dsc_t *icon = malloc(sizeof(lv_img_dsc_t) + data_size);
    if (icon == NULL)
    {
        free(pixels);
        return NULL;
    }
    lv_memset(icon, 0, sizeof(lv_img_dsc_t) + data_size);
    icon->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    icon->header.w = size;
    icon->header.h = size + 1;
    icon->data_size = data_size;
    icon->data = (const uint8_t *)&icon[1];
    uint32_t *id_row = (uint32_t *)icon->data;
    id_row[0] = ((uint32_t)(uintptr_t)icon >> 8) & 0x00FFFFFF;
    id_row[1] = (++icon_id) & 0x00FFFFFF;
    uint8_t *out = (uint8_t *)&id_row[size];

    // Box filter the image down to size x size. Colours are weighted by alpha so transparent
    // texels dont bleed dark edges into the icon.
    for (int oy = 0; oy < size; oy++)
    {
        int y0 = oy * h / size, y1 = LV_MAX(y0 + 1, (oy + 1) * h / size);
        for (int ox = 0; ox < size; ox++)
        {
            int x0 = ox * w / size, x1 = LV_MAX(x0 + 1, (ox + 1) * w / size);
            uint32_t a_sum = 0, r_sum = 0, g_sum = 0, b_sum = 0, cnt = 0;
            for (int y = y0; y < y1; y++)
            {
                for (int x = x0; x < x1; x++)
                {
                    uint32_t c = pixels[y * w + x];
                    uint32_t a = c >> 24;
                    a_sum += a;
                    r_sum += ((c >> 16) & 0xFF) * a;
                    g_sum += ((c >> 8) & 0xFF) * a;
                    b_sum += (c & 0xFF) * a;
                    cnt++;
                }
            }
            // LV_IMG_CF_TRUE_COLOR_ALPHA with 32bit colour depth is B,G,R,A in memory
            uint8_t *p = &out[(oy * size + ox) * 4];
            p[0] = (a_sum) ? (uint8_t)(b_sum / a_sum) : 0;
            p[1] = (a_sum) ? (uint8_t)(g_sum / a_sum) : 0;
            p[2] = (a_sum) ? (uint8_t)(r_sum / a_sum) : 0;
            p[3] = (uint8_t)(a_sum / cnt);
        }
    }
    free(pixels);
    return icon;
}

void dash_xbe_icon_free(lv_img_dsc_t *icon)
{
    free(icon);
}
