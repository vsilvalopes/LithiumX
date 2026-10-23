// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2022 Ryzee119

#ifndef _DASH_SETTINGS_H
#define _DASH_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lithiumx.h"

#define DASH_SETTINGS_VERSION 0x02
#define DASH_SETTINGS_MAGIC (0xBEEF0000+DASH_SETTINGS_VERSION)
typedef struct dash_settings {
    unsigned int magic;
    bool use_fahrenheit;
    bool auto_launch_dvd;
    bool show_debug_info;
    int startup_page_index;
    int theme_colour;
    int max_recent_items;
    int items_per_row;
    char earliest_recent_date[20]; //"YYYY-MM-DD HH:MM:SS"
    // Shrunk by sizeof(int) to make room for page_layout. This keeps the struct the same size so
    // settings saved by older versions still load. Their page_layout reads as 0 (DASH_LAYOUT_LIST).
    char sort_strings[4096 - sizeof(int)]; //Like "Games=1 Apps=1\0" etc.
    int page_layout; // DASH_LAYOUT_LIST or DASH_LAYOUT_WALL
} dash_settings_t;

#define DASH_LAYOUT_LIST 0
#define DASH_LAYOUT_WALL 1

void dash_settings_open(void);
void dash_settings_apply(bool confirm_box);
void dash_settings_read(void);

#ifdef __cplusplus
}
#endif

#endif
