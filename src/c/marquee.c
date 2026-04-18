#include "marquee.h"

#define TICK_MS         40
#define STEP_PX          2
#define END_PAUSE_TICKS 25    // ~1 s paused at each endpoint
#define MAX_MARQUEES     8

static Marquee *s_active[MAX_MARQUEES];
static int      s_active_count = 0;
static AppTimer *s_timer = NULL;

static void schedule_timer(void);

static int measure_width(const char *text, GFont font) {
  if (!text || !*text || !font) return 0;
  GSize sz = graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, 10000, 100),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  return sz.w;
}

static void compute_overflow(Marquee *m) {
  m->px_offset  = 0;
  m->pause      = END_PAUSE_TICKS;
  m->max_offset = 0;
  m->overflows  = false;
  m->text_width = 0;

  if (m->text_len == 0 || m->visible_w <= 0 || !m->font) return;

  m->text_width = measure_width(m->text, m->font);
  if (m->text_width <= m->visible_w) return;

  m->overflows  = true;
  m->max_offset = m->text_width - m->visible_w;
}

static void register_marquee(Marquee *m) {
  if (m->registered) return;
  if (s_active_count >= MAX_MARQUEES) return;
  s_active[s_active_count++] = m;
  m->registered = true;
  schedule_timer();
}

static void tick_cb(void *data) {
  s_timer = NULL;

  for (int i = 0; i < s_active_count; i++) {
    Marquee *m = s_active[i];
    bool changed = false;

    if (m->pause > 0) {
      m->pause--;
    } else if (m->px_offset < m->max_offset) {
      m->px_offset += STEP_PX;
      if (m->px_offset >= m->max_offset) {
        m->px_offset = m->max_offset;
        m->pause    = END_PAUSE_TICKS;
      }
      changed = true;
    } else {
      m->px_offset = 0;
      m->pause     = END_PAUSE_TICKS;
      changed = true;
    }

    if (changed && m->redraw) m->redraw(m->ctx);
  }

  if (s_active_count > 0) schedule_timer();
}

static void schedule_timer(void) {
  if (s_timer) return;
  if (s_active_count == 0) return;
  s_timer = app_timer_register(TICK_MS, tick_cb, NULL);
}

void marquee_set(Marquee *m, const char *text, GFont font, int visible_w,
                 MarqueeRedrawCallback redraw, void *ctx) {
  if (!m) return;

  // Don't restart the scroll if nothing that affects layout changed.
  // ui_set_track_info gets called every Spotify poll (~5 s) with the
  // same title; without this guard the marquee would snap back to
  // px_offset=0 before finishing even one full loop.
  const char *new_text = text ? text : "";
  bool same = m->font == font
           && m->visible_w == visible_w
           && strncmp(m->text, new_text, sizeof(m->text)) == 0;
  if (same) {
    m->redraw = redraw;
    m->ctx    = ctx;
    return;
  }

  strncpy(m->text, new_text, sizeof(m->text) - 1);
  m->text[sizeof(m->text) - 1] = '\0';
  m->text_len = strlen(m->text);
  m->font       = font;
  m->visible_w  = visible_w;
  m->redraw     = redraw;
  m->ctx        = ctx;

  compute_overflow(m);

  if (m->overflows) register_marquee(m);
  else              marquee_unregister(m);
}

void marquee_unregister(Marquee *m) {
  if (!m || !m->registered) return;
  for (int i = 0; i < s_active_count; i++) {
    if (s_active[i] == m) {
      for (int j = i; j < s_active_count - 1; j++) {
        s_active[j] = s_active[j + 1];
      }
      s_active_count--;
      break;
    }
  }
  m->registered = false;
  if (s_active_count == 0 && s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
}

// ------------------------------------------------------------------
// MarqueeLayer
// ------------------------------------------------------------------

struct MarqueeLayer {
  Layer *layer;
  Marquee m;
  GColor text_color;
  GTextAlignment alignment;  // used only when text fits
};

static void ml_redraw_cb(void *ctx) {
  MarqueeLayer *ml = ctx;
  if (ml && ml->layer) layer_mark_dirty(ml->layer);
}

static void ml_update_proc(Layer *layer, GContext *gctx) {
  MarqueeLayer *ml = *(MarqueeLayer **)layer_get_data(layer);
  if (!ml || !ml->m.font) return;
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_text_color(gctx, ml->text_color);

  if (ml->m.overflows) {
    // Draw at x = -px_offset; the layer's own bounds clip the text to
    // the visible window, so we just need a rect wide enough to fit the
    // entire string without wrapping.
    GRect r = GRect(-ml->m.px_offset, 0,
                    ml->m.text_width + 20, bounds.size.h);
    graphics_draw_text(gctx, ml->m.text, ml->m.font, r,
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
  } else {
    graphics_draw_text(gctx, ml->m.text, ml->m.font, bounds,
                       GTextOverflowModeTrailingEllipsis,
                       ml->alignment, NULL);
  }
}

MarqueeLayer *marquee_layer_create(GRect frame) {
  MarqueeLayer *ml = malloc(sizeof(MarqueeLayer));
  if (!ml) return NULL;
  ml->layer = layer_create_with_data(frame, sizeof(MarqueeLayer *));
  if (!ml->layer) { free(ml); return NULL; }
  *(MarqueeLayer **)layer_get_data(ml->layer) = ml;
  layer_set_update_proc(ml->layer, ml_update_proc);

  memset(&ml->m, 0, sizeof(ml->m));
  ml->m.visible_w = frame.size.w;
  ml->text_color  = GColorWhite;
  ml->alignment   = GTextAlignmentLeft;
  return ml;
}

void marquee_layer_destroy(MarqueeLayer *ml) {
  if (!ml) return;
  marquee_unregister(&ml->m);
  if (ml->layer) layer_destroy(ml->layer);
  free(ml);
}

Layer *marquee_layer_get_layer(MarqueeLayer *m) {
  return m ? m->layer : NULL;
}

void marquee_layer_set_font(MarqueeLayer *m, GFont font) {
  if (!m) return;
  m->m.font = font;
  // If text was already set, recompute widths with the new font.
  if (m->m.text_len > 0) {
    marquee_set(&m->m, m->m.text, font, m->m.visible_w, ml_redraw_cb, m);
  }
  if (m->layer) layer_mark_dirty(m->layer);
}

void marquee_layer_set_text_color(MarqueeLayer *m, GColor color) {
  if (!m) return;
  m->text_color = color;
  if (m->layer) layer_mark_dirty(m->layer);
}

void marquee_layer_set_alignment(MarqueeLayer *m, GTextAlignment align) {
  if (!m) return;
  m->alignment = align;
  if (m->layer) layer_mark_dirty(m->layer);
}

void marquee_layer_set_text(MarqueeLayer *m, const char *text) {
  if (!m) return;
  marquee_set(&m->m, text, m->m.font, m->m.visible_w, ml_redraw_cb, m);
  if (m->layer) layer_mark_dirty(m->layer);
}
