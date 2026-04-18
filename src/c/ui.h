#pragma once
#include <pebble.h>

void ui_init(Window *window);
void ui_deinit(void);
void ui_set_album_art(GBitmap *bitmap);
void ui_set_status(const char *text);
void ui_set_track_info(const char *title, const char *artist);
void ui_set_progress(int elapsed_sec, int total_sec);
void ui_set_shuffle(bool on);
// state: 0=off, 1=context, 2=track
void ui_set_repeat(int state);
