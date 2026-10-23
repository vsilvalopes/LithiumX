// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2022 Ryzee119

#ifndef _DASH_XBE_ICON_H
#define _DASH_XBE_ICON_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lithiumx.h"

// Reads the title image ($$XTIMAGE section, or $$XSIMAGE as a fallback) from an XBE. The image can be an
// XPR0 texture (retail/homebrew XBEs), a BMP (Repackinator/Cerbios attach XBEs) or a DDS. It is scaled to a
// size x size LV_IMG_CF_TRUE_COLOR_ALPHA image (plus one transparent row on top, see the .c file).
// The returned descriptor and its pixel data are a single allocation, free it with dash_xbe_icon_free().
// Returns NULL if the XBE has no usable image.
lv_img_dsc_t *dash_xbe_icon_load(const char *xbe_path, int size);
void dash_xbe_icon_free(lv_img_dsc_t *icon);

#ifdef __cplusplus
}
#endif

#endif
