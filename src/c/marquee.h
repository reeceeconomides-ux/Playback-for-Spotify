#pragma once
#include <pebble.h>

// Pixel-based horizontal text marquee.
//
// A shared AppTimer advances `px_offset` by STEP_PX every TICK_MS. Text
// is always drawn at x = -px_offset inside the visible window, so the
// scroll speed is constant regardless of glyph widths (which the older
// char-shift version tripped over — variable-width fonts made each tick
// jump a different distance and the output jittered).
//
// `Marquee` is the bare state struct consumed by custom draw code
// (menu cells). `MarqueeLayer` is a ready-made Layer that renders
// scrolling text, used where we'd otherwise use a `TextLayer`.

typedef void (*MarqueeRedrawCallback)(void *ctx);

typedef struct Marquee {
  char text[96];
  int  text_len;
  int  text_width;   // full measured pixel width
  int  visible_w;
  int  px_offset;    // current scroll offset (text drawn at x = -px_offset)
  int  max_offset;   // text_width - visible_w (0 when no overflow)
  int  pause;        // tick countdown at endpoints
  bool overflows;
  bool registered;
  GFont font;
  MarqueeRedrawCallback redraw;
  void *ctx;
} Marquee;

void marquee_set(Marquee *m, const char *text, GFont font, int visible_w,
                 MarqueeRedrawCallback redraw, void *ctx);
void marquee_unregister(Marquee *m);

// --- MarqueeLayer ---

typedef struct MarqueeLayer MarqueeLayer;

MarqueeLayer *marquee_layer_create(GRect frame);
void marquee_layer_destroy(MarqueeLayer *m);
Layer *marquee_layer_get_layer(MarqueeLayer *m);

// Configuration. set_font must be called before set_text (measurement
// needs the font).
void marquee_layer_set_font(MarqueeLayer *m, GFont font);
void marquee_layer_set_text_color(MarqueeLayer *m, GColor color);
// Alignment used when text fits; overflow mode ignores alignment and
// always left-aligns during scroll (see comment in marquee.c).
void marquee_layer_set_alignment(MarqueeLayer *m, GTextAlignment align);
void marquee_layer_set_text(MarqueeLayer *m, const char *text);
// Stop scrolling and reset to position 0.
void marquee_layer_stop(MarqueeLayer *m);
