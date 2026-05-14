#pragma once
#include <pebble.h>

void ui_init(Window *window);
void ui_deinit(void);
// Call after ui_init when launched via quick launch: blanks title/artist and
// starts a 5 s timer — if no track arrives, animates "No track playing" in.
void ui_start_loading(void);
void ui_set_album_art(GBitmap *bitmap);
#if !defined(PBL_PLATFORM_APLITE)
void ui_clear_album_art_instant(void);
#endif
void ui_set_status(const char *text);
void ui_set_track_info(const char *title, const char *artist);
void ui_set_progress(int elapsed_sec, int total_sec);
void ui_set_shuffle(bool on);
// state: 0=off, 1=context, 2=track
void ui_set_repeat(int state);
// Hint the direction of the next track-change animation. Forward (false)
// = exit left, enter from right (default). Reverse (true) = exit right,
// enter from left. Hint expires after ~3 s so it doesn't bleed into a
// later natural track change.
void ui_hint_animation_dir(bool reverse);
