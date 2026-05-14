#include "ui.h"
#include "comm.h"
#include "marquee.h"

static BitmapLayer *s_art_layer;
static MarqueeLayer *s_title_layer;
static TextLayer *s_artist_layer;
static TextLayer *s_time_layer;
static TextLayer *s_clock_layer;
static Layer *s_overlay_layer;
static Layer *s_progress_layer;
#if defined(PBL_ROUND)
static MarqueeLayer *s_title_shadow_layer;
static TextLayer *s_artist_shadow_layer;
static TextLayer *s_time_shadow_layer;
static TextLayer *s_clock_shadow_layer;
#endif

static char s_title_pending[64];    // incoming title (shown at animation midpoint)
static char s_artist_buf[64];       // incoming artist (written by ui_set_track_info)
static char s_artist_display_buf[64]; // artist shown on screen (pointer held by TextLayer)
static char s_time_buf[24];
static char s_clock_buf[8];
static int32_t s_elapsed_sec = 0;
static int32_t s_total_sec = 0;
static bool s_shuffle_on = false;
static int s_repeat_state = 0; // 0=off, 1=context, 2=track

// Animation direction hint. +1 = forward (exit left, enter from right),
// -1 = reverse (exit right, enter from left). Set via ui_hint_animation_dir.
// A timer resets to forward after HINT_TTL_MS so a stale "prev" hint
// doesn't apply to a later auto-poll track change.
//
// Disabled on aplite — the .bss budget is too tight (was 76 B free
// before this feature, ~180 B too small after). Aplite always animates
// forward; the public API is a no-op there.
#if !defined(PBL_PLATFORM_APLITE)
#define HINT_TTL_MS 3000
static int8_t s_hint_dir = 1;
static AppTimer *s_hint_timer = NULL;

static void hint_clear_cb(void *ctx) {
  s_hint_timer = NULL;
  s_hint_dir = 1;
}

static int8_t get_anim_dir(void) { return s_hint_dir; }
#endif

#define NP_MARQUEE_DURATION_MS 10000
static AppTimer *s_marquee_timer = NULL;
static void reset_marquee_timer(void);  // forward declaration (defined below draw helpers)

// ── Clock ───────────────────────────────────────────────────────────
static void update_clock_from_tm(struct tm *t) {
  strftime(s_clock_buf, sizeof(s_clock_buf),
           clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  // Strip leading zero in 12h mode ("09:30" → "9:30")
  if (!clock_is_24h_style() && s_clock_buf[0] == '0') {
    memmove(s_clock_buf, s_clock_buf + 1, strlen(s_clock_buf));
  }
  if (s_clock_layer) text_layer_set_text(s_clock_layer, s_clock_buf);
#if defined(PBL_ROUND)
  if (s_clock_shadow_layer) text_layer_set_text(s_clock_shadow_layer, s_clock_buf);
#endif
}

static void clock_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_clock_from_tm(tick_time);
}

static void update_clock(void) {
  time_t now = time(NULL);
  update_clock_from_tm(localtime(&now));
}

// ── Loading / "no track" deferred display ────────────────────────────────
#define NO_TRACK_TIMEOUT_MS  5000
#define TEXT_DEBOUNCE_MS      200   // batch rapid consecutive ui_set_track_info calls
static AppTimer *s_no_track_timer    = NULL;
static AppTimer *s_text_debounce_timer = NULL;
static bool s_track_received = false;

// ── Slide + spring transition ─────────────────────────────────────────────
#define TEXT_TRANSITION_MS 450
#define ART_TRANSITION_MS  450

// Home rects captured once in ui_init (overlay-local for text, root-local for art)
static int     s_screen_w;
static GRect   s_title_home;
static GRect   s_artist_home;
static GRect   s_art_home;
#if defined(PBL_ROUND)
static GRect   s_title_shadow_home;
static GRect   s_artist_shadow_home;
#endif

static bool       s_text_mid_done;
static GBitmap   *s_art_pending_bmp;
static bool       s_art_mid_done;

// Direction captured at the start of each animation. On aplite this is
// a compile-time constant (always forward) so the compiler folds away
// the multiplications and the bss is unaffected.
#if defined(PBL_PLATFORM_APLITE)
#define TEXT_ANIM_DIR 1
#define ART_ANIM_DIR  1
#else
static int8_t s_text_anim_dir = 1;
static int8_t s_art_anim_dir = 1;
#define TEXT_ANIM_DIR s_text_anim_dir
#define ART_ANIM_DIR  s_art_anim_dir
#endif
static Animation *s_text_anim;
static Animation *s_art_anim;

// ── Moook animation curve (ported from Pebble OS) ────────────────────────
// The Moook curve is a frame-table animation used throughout the Pebble OS UI.
// EXIT: ease-in quadratic slide off-screen left.
// ENTER: start at off-screen right, linear approach, then Moook-out bounce at home.
//   frames_in  = {0}         — start exactly at from (off-screen right)
//   mid frames = 2           — linear approach from off-right to near-home
//   frames_out = {8,4,2,1,0} — overshoot left then settle (characteristic Moook bounce)
#define MOOOK_FRAME_MS         33   // ANIMATION_TARGET_FRAME_INTERVAL_MS
#define MOOOK_BOUNCE           4
#define MOOOK_BOUNCE_EXT       (MOOOK_BOUNCE * 2)
#define MOOOK_EXIT_FRAMES      3    // exit: 3 frames ease-in
#define MOOOK_ENTER_MID_FRAMES 2    // enter: mid linear-approach frames

static const int32_t s_moook_enter_in[]  = {0};
static const int32_t s_moook_enter_out[] = {MOOOK_BOUNCE_EXT, MOOOK_BOUNCE, 2, 1, 0};

#define MOOOK_ENTER_IN_FRAMES  ((int)ARRAY_LENGTH(s_moook_enter_in))
#define MOOOK_ENTER_OUT_FRAMES ((int)ARRAY_LENGTH(s_moook_enter_out))
#define MOOOK_ENTER_FRAMES     (MOOOK_ENTER_IN_FRAMES + MOOOK_ENTER_MID_FRAMES \
                                + MOOOK_ENTER_OUT_FRAMES)
#define MOOOK_TOTAL_FRAMES     (MOOOK_EXIT_FRAMES + MOOOK_ENTER_FRAMES)

#undef TEXT_TRANSITION_MS
#undef ART_TRANSITION_MS
#define TEXT_TRANSITION_MS     (MOOOK_TOTAL_FRAMES * MOOOK_FRAME_MS)
#define ART_TRANSITION_MS      TEXT_TRANSITION_MS

// Normalized threshold where exit ends and enter begins (0..ANIMATION_NORMALIZED_MAX)
#define MOOOK_EXIT_THRESHOLD \
  ((int32_t)((int64_t)MOOOK_EXIT_FRAMES * ANIMATION_NORMALIZED_MAX / MOOOK_TOTAL_FRAMES))

// Map normalized 0..MAX to frame index 0..num_frames-1, with half-frame rounding.
static int32_t prv_moook_frame(int32_t normalized, int32_t num_frames) {
  int32_t MAX = ANIMATION_NORMALIZED_MAX;
  int32_t idx = (int32_t)(((int64_t)normalized * num_frames
                            + MAX / (2 * num_frames)) / MAX);
  return (idx < 0) ? 0 : (idx >= num_frames) ? num_frames - 1 : idx;
}

// Return delta from home for the ENTER phase.
//   from_dx: initial offset from home. Positive = approach from right
//   (forward gesture), negative = approach from left (reverse gesture).
//   Overshoot goes opposite to from_dx (continuing in the direction of
//   travel past home), then settles to 0.
static int32_t moook_enter_dx(int32_t normalized, int32_t from_dx) {
  int32_t MAX        = ANIMATION_NORMALIZED_MAX;
  int32_t num_total  = MOOOK_ENTER_FRAMES;
  if (normalized >= MAX) return 0;

  int32_t frame = prv_moook_frame(normalized, num_total);
  // overshoot_sign: -1 if approaching from right (overshoot left),
  //                 +1 if approaching from left  (overshoot right).
  // On aplite, hint is disabled so from_dx is always positive — folds to -1.
#if defined(PBL_PLATFORM_APLITE)
  const int32_t overshoot_sign = -1;
#else
  const int32_t overshoot_sign = (from_dx >= 0) ? -1 : 1;
#endif

  if (frame < MOOOK_ENTER_IN_FRAMES) {
    return from_dx;
  } else if (frame < MOOOK_ENTER_IN_FRAMES + MOOOK_ENTER_MID_FRAMES) {
    // mid: linear approach from from_dx to the first out-frame target
    int32_t shifted  = normalized
                       - (int32_t)(((int64_t)MOOOK_ENTER_IN_FRAMES * MAX) / num_total);
    int32_t mid_norm = (int32_t)(((int64_t)num_total * shifted) / MOOOK_ENTER_MID_FRAMES);
    int32_t end      = overshoot_sign * s_moook_enter_out[0];
    return from_dx + (int32_t)((int64_t)(end - from_dx) * mid_norm / MAX);
  } else {
    int32_t out_i = frame - MOOOK_ENTER_IN_FRAMES - MOOOK_ENTER_MID_FRAMES;
    return overshoot_sign * s_moook_enter_out[out_i];
  }
}

// ── Text layers ──────────────────────────────────────────────────────────

static void set_text_x_delta(int32_t dx) {
  GRect r;
  if (s_title_layer) {
    r = s_title_home; r.origin.x += dx;
    layer_set_frame(marquee_layer_get_layer(s_title_layer), r);
  }
  if (s_artist_layer) {
    r = s_artist_home; r.origin.x += dx;
    layer_set_frame(text_layer_get_layer(s_artist_layer), r);
  }
#if defined(PBL_ROUND)
  if (s_title_shadow_layer) {
    r = s_title_shadow_home; r.origin.x += dx;
    layer_set_frame(marquee_layer_get_layer(s_title_shadow_layer), r);
  }
  if (s_artist_shadow_layer) {
    r = s_artist_shadow_home; r.origin.x += dx;
    layer_set_frame(text_layer_get_layer(s_artist_shadow_layer), r);
  }
#endif
}

static void apply_text_content(void) {
  // Copy incoming artist into the display buffer so the TextLayer pointer
  // doesn't expose new content before the enter phase of the animation.
  strncpy(s_artist_display_buf, s_artist_buf, sizeof(s_artist_display_buf) - 1);
  s_artist_display_buf[sizeof(s_artist_display_buf) - 1] = '\0';

  if (s_title_layer)  marquee_layer_set_text(s_title_layer,  s_title_pending);
  if (s_artist_layer) text_layer_set_text(s_artist_layer, s_artist_display_buf);
#if defined(PBL_ROUND)
  if (s_title_shadow_layer)  marquee_layer_set_text(s_title_shadow_layer,  s_title_pending);
  if (s_artist_shadow_layer) text_layer_set_text(s_artist_shadow_layer, s_artist_display_buf);
#endif
  reset_marquee_timer();
}

#if !defined(PBL_PLATFORM_APLITE)
static void text_anim_update(Animation *anim, const AnimationProgress progress) {
  int32_t t   = (int32_t)progress;
  int32_t MAX = ANIMATION_NORMALIZED_MAX;

  if (t <= MOOOK_EXIT_THRESHOLD) {
    // EXIT: ease-in quadratic slide off in direction of travel
    int32_t exit_t = (MOOOK_EXIT_THRESHOLD > 0)
                     ? (int32_t)((int64_t)t * MAX / MOOOK_EXIT_THRESHOLD) : MAX;
    int32_t eased  = (int32_t)((int64_t)exit_t * exit_t / MAX);
    int32_t dx     = -TEXT_ANIM_DIR
                     * (int32_t)((int64_t)(s_screen_w + 10) * eased / MAX);
    set_text_x_delta(dx);
    s_text_mid_done = false;
  } else {
    // Swap content once when layers are fully off-screen
    if (!s_text_mid_done) {
      s_text_mid_done = true;
      apply_text_content();
    }
    // ENTER: Moook slide in from opposite side with bounce at home
    int32_t enter_t = (int32_t)((int64_t)(t - MOOOK_EXIT_THRESHOLD) * MAX
                                / (MAX - MOOOK_EXIT_THRESHOLD));
    int32_t from_dx = TEXT_ANIM_DIR * (s_screen_w + 10);
    set_text_x_delta(moook_enter_dx(enter_t, from_dx));
  }
}

static void text_anim_stopped(Animation *anim, bool finished, void *ctx) {
  s_text_anim = NULL;
  if (!s_text_mid_done) {
    // Cancelled before midpoint — still need to apply the new content
    apply_text_content();
  }
  set_text_x_delta(0);  // snap to home
}

static const AnimationImplementation s_text_anim_impl = {
  .update = text_anim_update,
};
#endif

static void start_text_transition(void) {
#if defined(PBL_PLATFORM_APLITE)
  // Aplite: skip animation entirely. Apply content immediately.
  // Frees the Animation* allocation, AppTimer, and avoids the 450 ms render
  // churn that hammers the font glyph cache on a 24 KB heap.
  set_text_x_delta(0);
  apply_text_content();
#else
  if (s_text_anim) {
    animation_unschedule(s_text_anim);  // fires stopped handler → s_text_anim = NULL
  }
  set_text_x_delta(0);
  s_text_mid_done = false;
  s_text_anim_dir = get_anim_dir();

  Animation *anim = animation_create();
  animation_set_duration(anim, TEXT_TRANSITION_MS);
  animation_set_curve(anim, AnimationCurveLinear);  // curve applied manually in update
  animation_set_implementation(anim, &s_text_anim_impl);
  animation_set_handlers(anim, (AnimationHandlers){ .stopped = text_anim_stopped }, NULL);
  animation_schedule(anim);
  s_text_anim = anim;
#endif
}

// ── Art layer ────────────────────────────────────────────────────────────

static void set_art_x(int32_t x) {
  if (!s_art_layer) return;
  GRect r = s_art_home;
  r.origin.x = x;
  layer_set_frame(bitmap_layer_get_layer(s_art_layer), r);
}

static void apply_art_content(void) {
  if (!s_art_layer) return;
  bitmap_layer_set_bitmap(s_art_layer, s_art_pending_bmp);
  layer_mark_dirty(bitmap_layer_get_layer(s_art_layer));
  comm_release_old_art(); // old bitmap is no longer on screen; safe to free
}

#if !defined(PBL_PLATFORM_APLITE)
static void art_anim_update(Animation *anim, const AnimationProgress progress) {
  int32_t t   = (int32_t)progress;
  int32_t MAX = ANIMATION_NORMALIZED_MAX;

  if (t <= MOOOK_EXIT_THRESHOLD) {
    int32_t exit_t = (MOOOK_EXIT_THRESHOLD > 0)
                     ? (int32_t)((int64_t)t * MAX / MOOOK_EXIT_THRESHOLD) : MAX;
    int32_t eased  = (int32_t)((int64_t)exit_t * exit_t / MAX);
    int32_t x      = s_art_home.origin.x
                     - ART_ANIM_DIR
                       * (int32_t)((int64_t)(s_screen_w + 10) * eased / MAX);
    set_art_x(x);
    s_art_mid_done = false;
  } else {
    if (!s_art_mid_done) {
      s_art_mid_done = true;
      apply_art_content();
    }
    int32_t enter_t = (int32_t)((int64_t)(t - MOOOK_EXIT_THRESHOLD) * MAX
                                / (MAX - MOOOK_EXIT_THRESHOLD));
    int32_t from_dx = ART_ANIM_DIR * (s_screen_w + 10);
    set_art_x(s_art_home.origin.x + moook_enter_dx(enter_t, from_dx));
  }
}

static void art_anim_stopped(Animation *anim, bool finished, void *ctx) {
  s_art_anim = NULL;
  if (!s_art_mid_done) {
    apply_art_content();
  }
  set_art_x(s_art_home.origin.x);  // snap to home
}

static const AnimationImplementation s_art_anim_impl = {
  .update = art_anim_update,
};
#endif

static void start_art_transition(GBitmap *bmp) {
  s_art_pending_bmp = bmp;  // set before unschedule so stopped handler sees latest bitmap
#if defined(PBL_PLATFORM_APLITE)
  // Aplite: apply directly without animation (heap too tight for animation runtime).
  apply_art_content();
  set_art_x(s_art_home.origin.x);
#else
  if (s_art_anim) {
    animation_unschedule(s_art_anim);
  }
  set_art_x(s_art_home.origin.x);
  s_art_mid_done = false;
  s_art_anim_dir = get_anim_dir();

  Animation *anim = animation_create();
  animation_set_duration(anim, ART_TRANSITION_MS);
  animation_set_curve(anim, AnimationCurveLinear);
  animation_set_implementation(anim, &s_art_anim_impl);
  animation_set_handlers(anim, (AnimationHandlers){ .stopped = art_anim_stopped }, NULL);
  animation_schedule(anim);
  s_art_anim = anim;
#endif
}

static void marquee_stop_cb(void *data) {
  s_marquee_timer = NULL;
  marquee_layer_stop(s_title_layer);
#if defined(PBL_ROUND)
  marquee_layer_stop(s_title_shadow_layer);
#endif
}

static void reset_marquee_timer(void) {
  if (s_marquee_timer) {
    app_timer_reschedule(s_marquee_timer, NP_MARQUEE_DURATION_MS);
  } else {
    s_marquee_timer = app_timer_register(NP_MARQUEE_DURATION_MS, marquee_stop_cb, NULL);
  }
}

// 8x8 shuffle/repeat icons — only used on rectangular displays.
#if !defined(PBL_ROUND)
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
#endif // !PBL_ROUND

static void overlay_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
#if defined(PBL_ROUND)
  graphics_context_set_stroke_color(ctx, GColorBlack);
  for (int y = 0; y < bounds.size.h; y++) {
    for (int x = 0; x < bounds.size.w; x++) {
      if ((x & 1) || (y & 1)) {
        graphics_draw_pixel(ctx, GPoint(x, y));
      }
    }
  }
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
#endif

#if !defined(PBL_ROUND)
  int icon_y = bounds.size.h - 12;
  GSize time_size = graphics_text_layout_get_content_size(
      s_time_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(0, 0, bounds.size.w, bounds.size.h),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int icon_x = 4 + time_size.w + 4; // 4px left margin + text + 4px gap
  if (s_shuffle_on) {
    draw_shuffle_icon(ctx, icon_x, icon_y);
    icon_x += 11;
  }
  if (s_repeat_state > 0) {
    draw_repeat_icon(ctx, icon_x, icon_y, s_repeat_state);
  }
#endif
}

static void progress_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Unfilled background
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Filled portion
  int filled_w = (s_total_sec > 0)
    ? (int)((int64_t)bounds.size.w * s_elapsed_sec / s_total_sec)
    : 0;
  if (filled_w > bounds.size.w) filled_w = bounds.size.w;
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

  // Seed buffers with defaults on very first open so dedup works correctly.
  // On reopen these already hold the last known track — layers will be
  // initialized to that text so the dedup check stays in sync.
  if (!s_title_pending[0]) {
    strncpy(s_title_pending, "Playback", sizeof(s_title_pending) - 1);
    strncpy(s_artist_buf, "No track playing", sizeof(s_artist_buf) - 1);
  }
  // Keep display buffer in sync with incoming buffer so TextLayer init shows
  // the right content.
  strncpy(s_artist_display_buf, s_artist_buf, sizeof(s_artist_display_buf) - 1);
  s_artist_display_buf[sizeof(s_artist_display_buf) - 1] = '\0';

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
  marquee_layer_set_text(s_title_shadow_layer, s_title_pending);
  layer_add_child(s_overlay_layer, marquee_layer_get_layer(s_title_shadow_layer));

  s_title_layer = marquee_layer_create(GRect(text_inset, title_y, text_w, title_h));
  marquee_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  marquee_layer_set_text_color(s_title_layer, GColorWhite);
  marquee_layer_set_alignment(s_title_layer, GTextAlignmentCenter);
  marquee_layer_set_text(s_title_layer, s_title_pending);
  layer_add_child(s_overlay_layer, marquee_layer_get_layer(s_title_layer));

  // --- Artist (with shadow). Static text — no marquee, just ellipsize. ---
  s_artist_shadow_layer = text_layer_create(GRect(text_inset + 1, artist_y + 1, text_w, artist_h));
  text_layer_set_background_color(s_artist_shadow_layer, GColorClear);
  text_layer_set_text_color(s_artist_shadow_layer, GColorBlack);
  text_layer_set_font(s_artist_shadow_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_artist_shadow_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_artist_shadow_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_text(s_artist_shadow_layer, s_artist_display_buf);
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_artist_shadow_layer));

  s_artist_layer = text_layer_create(GRect(text_inset, artist_y, text_w, artist_h));
  text_layer_set_background_color(s_artist_layer, GColorClear);
  text_layer_set_text_color(s_artist_layer, GColorWhite);
  text_layer_set_font(s_artist_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_artist_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_artist_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_text(s_artist_layer, s_artist_display_buf);
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

  // Clock dropped on round — the 38px slot fights with the centered progress
  // text in the curved bottom band; the progress info is the more useful of
  // the two. Tick subscription and deinit are gated to match.

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
  marquee_layer_set_text(s_title_layer, s_title_pending);
  layer_add_child(s_overlay_layer, marquee_layer_get_layer(s_title_layer));

  s_artist_layer = text_layer_create(GRect(4, 21, W - 8, 18));
  text_layer_set_background_color(s_artist_layer, GColorClear);
  text_layer_set_text_color(s_artist_layer, GColorWhite);
  text_layer_set_font(s_artist_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_overflow_mode(s_artist_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_text(s_artist_layer, s_artist_display_buf);
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_artist_layer));

  s_progress_layer = layer_create(GRect(4, 39, W - 8, 4));
  layer_set_update_proc(s_progress_layer, progress_update_proc);
  layer_add_child(s_overlay_layer, s_progress_layer);

  s_time_layer = text_layer_create(GRect(4, 44, W - 48, 16));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorLightGray);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_time_layer, "0:00 / 0:00");
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_time_layer));

#if !defined(PBL_PLATFORM_APLITE)
  s_clock_layer = text_layer_create(GRect(W - 40, 42, 36, 20));
  text_layer_set_background_color(s_clock_layer, GColorClear);
  text_layer_set_text_color(s_clock_layer, GColorWhite);
  text_layer_set_font(s_clock_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_clock_layer, GTextAlignmentRight);
  text_layer_set_text(s_clock_layer, s_clock_buf);
  layer_add_child(s_overlay_layer, text_layer_get_layer(s_clock_layer));
#endif
#endif

  snprintf(s_time_buf, sizeof(s_time_buf), "0:00 / 0:00");

  // Capture home rects once so the transition functions can reference them
  s_screen_w   = W;
  s_title_home  = layer_get_frame(marquee_layer_get_layer(s_title_layer));
  s_artist_home = layer_get_frame(text_layer_get_layer(s_artist_layer));
  s_art_home    = layer_get_frame(bitmap_layer_get_layer(s_art_layer));
#if defined(PBL_ROUND)
  s_title_shadow_home  = layer_get_frame(marquee_layer_get_layer(s_title_shadow_layer));
  s_artist_shadow_home = layer_get_frame(text_layer_get_layer(s_artist_shadow_layer));
#endif

  reset_marquee_timer();
#if !defined(PBL_PLATFORM_APLITE) && !defined(PBL_ROUND)
  update_clock();
  tick_timer_service_subscribe(MINUTE_UNIT, clock_tick_handler);
#endif
}

void ui_deinit(void) {
  // Cancel any running transitions before destroying layers
  if (s_text_anim) {
    animation_unschedule(s_text_anim);  // stopped handler snaps layers to home
  }
  if (s_art_anim) {
    animation_unschedule(s_art_anim);
  }
#if !defined(PBL_PLATFORM_APLITE) && !defined(PBL_ROUND)
  tick_timer_service_unsubscribe();
  text_layer_destroy(s_clock_layer);
  s_clock_layer = NULL;
#endif
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
  if (s_marquee_timer) {
    app_timer_cancel(s_marquee_timer);
    s_marquee_timer = NULL;
  }
  if (s_no_track_timer) {
    app_timer_cancel(s_no_track_timer);
    s_no_track_timer = NULL;
  }
  if (s_text_debounce_timer) {
    app_timer_cancel(s_text_debounce_timer);
    s_text_debounce_timer = NULL;
  }
}

void ui_set_album_art(GBitmap *bitmap) {
  if (!s_art_layer) return;
#if !defined(PBL_PLATFORM_APLITE)
  if (!bitmap) {
    ui_clear_album_art_instant();
    return;
  }
#endif
  start_art_transition(bitmap);
}

#if !defined(PBL_PLATFORM_APLITE)
void ui_clear_album_art_instant(void) {
  if (!s_art_layer) return;
  s_art_pending_bmp = NULL;
  if (s_art_anim) {
    animation_unschedule(s_art_anim);
  }
  bitmap_layer_set_bitmap(s_art_layer, NULL);
}
#endif

void ui_set_status(const char *text) {
  (void)text; // Status is not shown in the Now Playing UI
}

static void text_debounce_cb(void *ctx) {
  s_text_debounce_timer = NULL;
  start_text_transition();
}

static void no_track_timeout_cb(void *ctx) {
  s_no_track_timer = NULL;
  if (s_track_received || !s_title_layer) return;
  // Animate "No track playing" in from the right (against clean black background)
  strncpy(s_title_pending, "Playback", sizeof(s_title_pending) - 1);
  s_title_pending[sizeof(s_title_pending) - 1] = '\0';
  strncpy(s_artist_buf, "No track playing", sizeof(s_artist_buf) - 1);
  s_artist_buf[sizeof(s_artist_buf) - 1] = '\0';
  start_text_transition();
}

void ui_start_loading(void) {
  if (!s_title_layer) return;
  if (s_text_debounce_timer) {
    app_timer_cancel(s_text_debounce_timer);
    s_text_debounce_timer = NULL;
  }
  // Blank both text layers immediately so the screen starts clean
  s_title_pending[0] = '\0';
  s_artist_buf[0] = '\0';
  apply_text_content();
  s_track_received = false;
  if (s_no_track_timer) {
    app_timer_cancel(s_no_track_timer);
  }
  s_no_track_timer = app_timer_register(NO_TRACK_TIMEOUT_MS, no_track_timeout_cb, NULL);
}

void ui_set_track_info(const char *title, const char *artist) {
  // Cancel the loading timeout — real data has arrived
  if (s_no_track_timer) {
    app_timer_cancel(s_no_track_timer);
    s_no_track_timer = NULL;
  }
  s_track_received = true;

  bool changed = (strcmp(title, s_title_pending) != 0
               || strcmp(artist, s_artist_buf) != 0);

  strncpy(s_title_pending, title, sizeof(s_title_pending) - 1);
  s_title_pending[sizeof(s_title_pending) - 1] = '\0';
  strncpy(s_artist_buf, artist, sizeof(s_artist_buf) - 1);
  s_artist_buf[sizeof(s_artist_buf) - 1] = '\0';

  if (!s_title_layer || !changed) return;

  // Debounce: if two rapid updates arrive (e.g. title then artist as separate
  // messages), batch them into a single animation.
  if (s_text_debounce_timer) {
    app_timer_reschedule(s_text_debounce_timer, TEXT_DEBOUNCE_MS);
  } else {
    s_text_debounce_timer = app_timer_register(TEXT_DEBOUNCE_MS,
                                               text_debounce_cb, NULL);
  }
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

void ui_hint_animation_dir(bool reverse) {
#if defined(PBL_PLATFORM_APLITE)
  (void)reverse;  // aplite always animates forward (RAM-constrained)
#else
  s_hint_dir = reverse ? -1 : 1;
  if (s_hint_timer) {
    app_timer_reschedule(s_hint_timer, HINT_TTL_MS);
  } else {
    s_hint_timer = app_timer_register(HINT_TTL_MS, hint_clear_cb, NULL);
  }
#endif
}

void ui_set_progress(int elapsed_sec, int total_sec) {
  s_elapsed_sec = (elapsed_sec < 0) ? 0 : elapsed_sec;
  s_total_sec = total_sec;
  if (s_total_sec > 0 && s_elapsed_sec > s_total_sec) s_elapsed_sec = s_total_sec;

  int e_min = elapsed_sec / 60, e_sec = elapsed_sec % 60;
  int t_min = total_sec / 60, t_sec = total_sec % 60;
  snprintf(s_time_buf, sizeof(s_time_buf), "%d:%02d / %d:%02d",
           e_min, e_sec, t_min, t_sec);

  if (!s_progress_layer) return;
  layer_mark_dirty(s_progress_layer);

  text_layer_set_text(s_time_layer, s_time_buf);
  layer_mark_dirty(text_layer_get_layer(s_time_layer));
#if defined(PBL_ROUND)
  if (s_time_shadow_layer) {
    text_layer_set_text(s_time_shadow_layer, s_time_buf);
    layer_mark_dirty(text_layer_get_layer(s_time_shadow_layer));
  }
#endif
}
