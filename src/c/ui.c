#include "ui.h"
#include "marquee.h"

static BitmapLayer *s_art_layer;
static MarqueeLayer *s_title_layer;
static TextLayer *s_artist_layer;
static TextLayer *s_time_layer;
static Layer *s_overlay_layer;
static Layer *s_progress_layer;
#if defined(PBL_ROUND)
// Shadow copies drawn 1px offset behind the main labels so white text
// stays readable on top of the dithered album art.
static MarqueeLayer *s_title_shadow_layer;
static TextLayer *s_artist_shadow_layer;
static TextLayer *s_time_shadow_layer;
#endif

static char s_title_buf[64];
static char s_artist_buf[64];
static char s_time_buf[24];
static char s_status_buf_ui[64];
static bool s_showing_status = false;
static AppTimer *s_status_timer = NULL;
static float s_progress = 0.0f;
static bool s_shuffle_on = false;
static int s_repeat_state = 0; // 0=off, 1=context, 2=track

// 8x8 shuffle icon — two crossing arrows, only drawn when shuffle is on.
static void draw_shuffle_icon(GContext *ctx, int x, int y) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_line(ctx, GPoint(x,     y + 1), GPoint(x + 6, y + 6));
  graphics_draw_line(ctx, GPoint(x,     y + 6), GPoint(x + 6, y + 1));
  // arrowheads at the right tips
  graphics_draw_line(ctx, GPoint(x + 7, y),     GPoint(x + 7, y + 2));
  graphics_draw_line(ctx, GPoint(x + 5, y),     GPoint(x + 7, y));
  graphics_draw_line(ctx, GPoint(x + 7, y + 5), GPoint(x + 7, y + 7));
  graphics_draw_line(ctx, GPoint(x + 5, y + 7), GPoint(x + 7, y + 7));
}

// 8x8 repeat icon — loop rectangle with an arrow notch, optional "1"
// inside for repeat-track. Not drawn when state == 0.
static void draw_repeat_icon(GContext *ctx, int x, int y, int state) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  // Loop body (top-right broken by the arrow)
  graphics_draw_line(ctx, GPoint(x,     y + 1), GPoint(x + 5, y + 1));
  graphics_draw_line(ctx, GPoint(x,     y + 1), GPoint(x,     y + 6));
  graphics_draw_line(ctx, GPoint(x,     y + 6), GPoint(x + 7, y + 6));
  graphics_draw_line(ctx, GPoint(x + 7, y + 3), GPoint(x + 7, y + 6));
  // Arrow pointing in to close the loop
  graphics_draw_line(ctx, GPoint(x + 5, y),     GPoint(x + 7, y + 2));
  graphics_draw_line(ctx, GPoint(x + 5, y + 3), GPoint(x + 7, y + 2));
  // "1" glyph inside for repeat-track
  if (state == 2) {
    graphics_draw_line(ctx, GPoint(x + 3, y + 2), GPoint(x + 3, y + 5));
    graphics_draw_pixel(ctx, GPoint(x + 2, y + 3));
  }
}

static void status_clear_cb(void *data) {
  s_status_timer = NULL;
  s_showing_status = false;
  text_layer_set_text(s_time_layer, s_time_buf);
  layer_mark_dirty(text_layer_get_layer(s_time_layer));
#if defined(PBL_ROUND)
  if (s_time_shadow_layer) {
    text_layer_set_text(s_time_shadow_layer, s_time_buf);
    layer_mark_dirty(text_layer_get_layer(s_time_shadow_layer));
  }
#endif
}

static void overlay_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
#if defined(PBL_ROUND)
  // 75% dither over the album art: draw black on 3 of every 4 pixels,
  // leaving 1 of 4 transparent so the art bleeds through as a darkened
  // background. Text drawn on top still reads clearly.
  graphics_context_set_stroke_color(ctx, GColorBlack);
  for (int y = 0; y < bounds.size.h; y++) {
    for (int x = 0; x < bounds.size.w; x++) {
      // Keep every (even, even) pixel untouched; paint the other 3.
      if ((x & 1) || (y & 1)) {
        graphics_draw_pixel(ctx, GPoint(x, y));
      }
    }
  }
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
#endif

  // Shuffle/repeat icons in the bottom-right corner of the overlay.
  // Rectangular-only — round displays are too tight with the current
  // overlay layout to fit extra glyphs without clobbering the time.
#if !defined(PBL_ROUND)
  int icon_y = bounds.size.h - 12;
  if (s_shuffle_on) {
    draw_shuffle_icon(ctx, bounds.size.w - 22, icon_y);
  }
  if (s_repeat_state > 0) {
    draw_repeat_icon(ctx, bounds.size.w - 11, icon_y, s_repeat_state);
  }
#endif
}


static void progress_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Unfilled background
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Filled portion
  int filled_w = (int)(bounds.size.w * s_progress);
  if (filled_w > 0) {
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite));
    graphics_fill_rect(ctx, GRect(0, 0, filled_w, bounds.size.h), 0, GCornerNone);
  }
}

void ui_init(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  int W = bounds.size.w;
  int H = bounds.size.h;

#if defined(PBL_ROUND)
  // Round layout (Chalk 180 / Gabbro 260): album art fills the entire
  // screen and a dithered-darkened band at the bottom holds the track
  // info. Text is drawn on top of the art with a 1px black shadow.

  // Per-platform layout numbers — chalk is smaller so the overlay has
  // to be shorter and the text pulled inward from the circle edges.
  #if defined(PBL_PLATFORM_CHALK)
    int overlay_h   = 70;
    int text_inset  = 20;
    int title_y     = 2;
    int title_h     = 22;
    int artist_y    = 24;
    int artist_h    = 16;
    int prog_w      = 60;
    int prog_y      = 44;
    int time_y      = 48;
    int time_h      = 16;
  #else  // gabbro and any future round platform
    int overlay_h   = 88;
    int text_inset  = 30;
    int title_y     = 6;
    int title_h     = 24;
    int artist_y    = 32;
    int artist_h    = 20;
    int prog_w      = 90;
    int prog_y      = 58;
    int time_y      = 64;
    int time_h      = 20;
  #endif
  int text_w = W - 2 * text_inset;

  // Album art fills the full screen — it's now the background behind
  // the overlay, not a cropped top half.
  s_art_layer = bitmap_layer_create(GRect(0, 0, W, H));
  bitmap_layer_set_alignment(s_art_layer, GAlignCenter);
  bitmap_layer_set_background_color(s_art_layer, GColorBlack);
  bitmap_layer_set_compositing_mode(s_art_layer, GCompOpSet);
  layer_add_child(root, bitmap_layer_get_layer(s_art_layer));

  // Dithered overlay sits at the bottom, on top of the art.
  s_overlay_layer = layer_create(GRect(0, H - overlay_h, W, overlay_h));
  layer_set_update_proc(s_overlay_layer, overlay_update_proc);
  layer_add_child(root, s_overlay_layer);

  // --- Title (with 1px black shadow behind it) ---
  s_title_shadow_layer = marquee_layer_create(GRect(text_inset + 1, title_y + 1, text_w, title_h));
  marquee_layer_set_font(s_title_shadow_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  marquee_layer_set_text_color(s_title_shadow_layer, GColorBlack);
  marquee_layer_set_alignment(s_title_shadow_layer, GTextAlignmentCenter);
  marquee_layer_set_text(s_title_shadow_layer, "Playback");
  layer_add_child(s_overlay_layer, marquee_layer_get_layer(s_title_shadow_layer));

  s_title_layer = marquee_layer_create(GRect(text_inset, title_y, text_w, title_h));
  marquee_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  marquee_layer_set_text_color(s_title_layer, GColorWhite);
  marquee_layer_set_alignment(s_title_layer, GTextAlignmentCenter);
  marquee_layer_set_text(s_title_layer, "Playback");
  layer_add_child(s_overlay_layer, marquee_layer_get_layer(s_title_layer));

  // --- Artist (with shadow). Static text — no marquee, just ellipsize. ---
  s_artist_shadow_layer = text_layer_create(GRect(text_inset + 1, artist_y + 1, text_w, artist_h));
  text_layer_set_background_color(s_artist_shadow_layer, GColorClear);
  text_layer_set_text_color(s_artist_shadow_layer, GColorBlack);
  text_layer_set_font(s_artist_shadow_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_artist_shadow_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_artist_shadow_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_text(s_artist_shadow_layer, "No track");
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_artist_shadow_layer));

  s_artist_layer = text_layer_create(GRect(text_inset, artist_y, text_w, artist_h));
  text_layer_set_background_color(s_artist_layer, GColorClear);
  text_layer_set_text_color(s_artist_layer, GColorWhite);
  text_layer_set_font(s_artist_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_artist_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_artist_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_text(s_artist_layer, "No track");
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_artist_layer));

  // --- Progress bar (short, centered so it doesn't run into the curve) ---
  s_progress_layer = layer_create(GRect((W - prog_w) / 2, prog_y, prog_w, 4));
  layer_set_update_proc(s_progress_layer, progress_update_proc);
  layer_add_child(s_overlay_layer, s_progress_layer);

  // --- Time (with shadow) ---
  s_time_shadow_layer = text_layer_create(GRect(text_inset + 1, time_y + 1, text_w, time_h));
  text_layer_set_background_color(s_time_shadow_layer, GColorClear);
  text_layer_set_text_color(s_time_shadow_layer, GColorBlack);
  text_layer_set_font(s_time_shadow_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_time_shadow_layer, GTextAlignmentCenter);
  text_layer_set_text(s_time_shadow_layer, "0:00 / 0:00");
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_time_shadow_layer));

  s_time_layer = text_layer_create(GRect(text_inset, time_y, text_w, time_h));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_text(s_time_layer, "0:00 / 0:00");
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_time_layer));

#else
  // Original Rectangular Layout
  int overlay_h = 62;
  int art_h = H - overlay_h;

  s_art_layer = bitmap_layer_create(GRect(0, 0, W, art_h));
  bitmap_layer_set_alignment(s_art_layer, GAlignCenter);
  bitmap_layer_set_background_color(s_art_layer, GColorBlack);
  bitmap_layer_set_compositing_mode(s_art_layer, GCompOpSet);
  layer_add_child(root, bitmap_layer_get_layer(s_art_layer));

  s_overlay_layer = layer_create(GRect(0, art_h, W, overlay_h));
  layer_set_update_proc(s_overlay_layer, overlay_update_proc);
  layer_add_child(root, s_overlay_layer);

  s_title_layer = marquee_layer_create(GRect(4, 2, W - 8, 20));
  marquee_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  marquee_layer_set_text_color(s_title_layer, GColorWhite);
  marquee_layer_set_alignment(s_title_layer, GTextAlignmentLeft);
  marquee_layer_set_text(s_title_layer, "Playback");
  layer_add_child(s_overlay_layer, marquee_layer_get_layer(s_title_layer));

  s_artist_layer = text_layer_create(GRect(4, 21, W - 8, 18));
  text_layer_set_background_color(s_artist_layer, GColorClear);
  text_layer_set_text_color(s_artist_layer, GColorWhite);
  text_layer_set_font(s_artist_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_overflow_mode(s_artist_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_text(s_artist_layer, "No track playing");
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_artist_layer));

  s_progress_layer = layer_create(GRect(4, 39, W - 8, 4));
  layer_set_update_proc(s_progress_layer, progress_update_proc);
  layer_add_child(s_overlay_layer, s_progress_layer);

  s_time_layer = text_layer_create(GRect(4, 44, W - 8, 16));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorLightGray);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_time_layer, "0:00 / 0:00");
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_time_layer));
#endif

  snprintf(s_time_buf, sizeof(s_time_buf), "0:00 / 0:00");
}

void ui_deinit(void) {
  if (s_status_timer) {
    app_timer_cancel(s_status_timer);
    s_status_timer = NULL;
  }
  text_layer_destroy(s_time_layer);
  s_time_layer = NULL;
#if defined(PBL_ROUND)
  text_layer_destroy(s_time_shadow_layer);
  s_time_shadow_layer = NULL;
#endif
  layer_destroy(s_progress_layer);
  s_progress_layer = NULL;
  text_layer_destroy(s_artist_layer);
  s_artist_layer = NULL;
#if defined(PBL_ROUND)
  text_layer_destroy(s_artist_shadow_layer);
  s_artist_shadow_layer = NULL;
#endif
  marquee_layer_destroy(s_title_layer);
  s_title_layer = NULL;
#if defined(PBL_ROUND)
  marquee_layer_destroy(s_title_shadow_layer);
  s_title_shadow_layer = NULL;
#endif
  layer_destroy(s_overlay_layer);
  s_overlay_layer = NULL;
  bitmap_layer_destroy(s_art_layer);
  s_art_layer = NULL;
}

void ui_set_album_art(GBitmap *bitmap) {
  if (!s_art_layer) return;
  bitmap_layer_set_bitmap(s_art_layer, bitmap);
  layer_mark_dirty(bitmap_layer_get_layer(s_art_layer));
}

void ui_set_status(const char *text) {
  strncpy(s_status_buf_ui, text, sizeof(s_status_buf_ui) - 1);
  s_status_buf_ui[sizeof(s_status_buf_ui) - 1] = '\0';
  s_showing_status = true;

  if (!s_time_layer) return;
  text_layer_set_text(s_time_layer, s_status_buf_ui);
#if defined(PBL_ROUND)
  if (s_time_shadow_layer) text_layer_set_text(s_time_shadow_layer, s_status_buf_ui);
#endif

  // Auto-clear status after 3 seconds
  if (s_status_timer) app_timer_cancel(s_status_timer);
  s_status_timer = app_timer_register(3000, status_clear_cb, NULL);
}

void ui_set_track_info(const char *title, const char *artist) {
  strncpy(s_title_buf, title, sizeof(s_title_buf) - 1);
  s_title_buf[sizeof(s_title_buf) - 1] = '\0';
  strncpy(s_artist_buf, artist, sizeof(s_artist_buf) - 1);
  s_artist_buf[sizeof(s_artist_buf) - 1] = '\0';

  if (!s_title_layer) return;
  marquee_layer_set_text(s_title_layer, s_title_buf);
  text_layer_set_text(s_artist_layer, s_artist_buf);
  layer_mark_dirty(text_layer_get_layer(s_artist_layer));
#if defined(PBL_ROUND)
  // Keep shadow copies in sync so the darker glyph sits 1 px offset
  // behind the white one.
  if (s_title_shadow_layer)  marquee_layer_set_text(s_title_shadow_layer, s_title_buf);
  if (s_artist_shadow_layer) {
    text_layer_set_text(s_artist_shadow_layer, s_artist_buf);
    layer_mark_dirty(text_layer_get_layer(s_artist_shadow_layer));
  }
#endif
}

void ui_set_shuffle(bool on) {
  if (s_shuffle_on == on) return;
  s_shuffle_on = on;
  if (s_overlay_layer) layer_mark_dirty(s_overlay_layer);
}

void ui_set_repeat(int state) {
  if (s_repeat_state == state) return;
  s_repeat_state = state;
  if (s_overlay_layer) layer_mark_dirty(s_overlay_layer);
}

void ui_set_progress(int elapsed_sec, int total_sec) {
  s_progress = (total_sec > 0) ? (float)elapsed_sec / total_sec : 0.0f;
  if (s_progress > 1.0f) s_progress = 1.0f;

  int e_min = elapsed_sec / 60, e_sec = elapsed_sec % 60;
  int t_min = total_sec / 60, t_sec = total_sec % 60;
  snprintf(s_time_buf, sizeof(s_time_buf), "%d:%02d / %d:%02d",
           e_min, e_sec, t_min, t_sec);

  if (!s_progress_layer) return;
  layer_mark_dirty(s_progress_layer);

  if (!s_showing_status) {
    text_layer_set_text(s_time_layer, s_time_buf);
    layer_mark_dirty(text_layer_get_layer(s_time_layer));
#if defined(PBL_ROUND)
    if (s_time_shadow_layer) {
      text_layer_set_text(s_time_shadow_layer, s_time_buf);
      layer_mark_dirty(text_layer_get_layer(s_time_shadow_layer));
    }
#endif
  }
}
