#include <pebble.h>
#include "touch_input.h"

#if HAS_TOUCH

#define MAX_HANDLERS 6

typedef struct {
  Window *win;
  GestureHandler handler;
  void *context;
} HandlerEntry;

static HandlerEntry s_handlers[MAX_HANDLERS];

static HandlerEntry *find_entry(Window *win) {
  for (int i = 0; i < MAX_HANDLERS; i++) {
    if (s_handlers[i].win == win) return &s_handlers[i];
  }
  return NULL;
}

// Pixel thresholds. Tuned for a 200x228 (emery) and 173x173 (gabbro)
// display — finger taps land within a ~10px circle, deliberate swipes
// cover at least ~30px. Anything in between is ambiguous and dropped.
#define TAP_MAX_DISPLACEMENT   12
#define SWIPE_MIN_DISPLACEMENT 30
// Dominant axis must be at least 1.7x the other to count as a swipe.
#define SWIPE_DOMINANCE_NUM    17
#define SWIPE_DOMINANCE_DEN    10
// Drag-scroll threshold: every ROW_STEP_PX of vertical motion advances
// the selection by one row. Larger = slower scroll (more finger travel
// per row). Tuned by feel on real hardware.
#define ROW_STEP_PX            45

static int16_t s_start_x = 0;
static int16_t s_start_y = 0;
static int16_t s_last_step_y = 0;
static bool s_active = false;
static bool s_dragged = false;
static bool s_subscribed = false;

static void dispatch(GestureKind kind) {
  Window *top = window_stack_get_top_window();
  if (!top) return;
  HandlerEntry *e = find_entry(top);
  if (e && e->handler) e->handler(kind, e->context);
}

static void touch_handler(const TouchEvent *event, void *context) {
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_start_x = event->x;
      s_start_y = event->y;
      s_last_step_y = event->y;
      s_active = true;
      s_dragged = false;
      break;
    case TouchEvent_PositionUpdate: {
      if (!s_active) return;
      // Drag-scroll: fire one step per ROW_STEP_PX of vertical motion.
      // Loop in case a fast drag jumped multiple rows in one update.
      int dy = (int)event->y - (int)s_last_step_y;
      while (dy >= ROW_STEP_PX) {
        s_dragged = true;
        s_last_step_y += ROW_STEP_PX;
        dy -= ROW_STEP_PX;
        // Finger moved DOWN — content moves down — selection moves UP.
        dispatch(GESTURE_SCROLL_DOWN);
      }
      while (dy <= -ROW_STEP_PX) {
        s_dragged = true;
        s_last_step_y -= ROW_STEP_PX;
        dy += ROW_STEP_PX;
        // Finger moved UP — content moves up — selection moves DOWN.
        dispatch(GESTURE_SCROLL_UP);
      }
      break;
    }
    case TouchEvent_Liftoff: {
      if (!s_active) return;
      s_active = false;
      // If the touch was already classified as a drag, swallow Liftoff
      // — no tap or swipe should fire.
      if (s_dragged) {
        s_dragged = false;
        return;
      }
      int dx = (int)event->x - (int)s_start_x;
      int dy = (int)event->y - (int)s_start_y;
      int adx = dx < 0 ? -dx : dx;
      int ady = dy < 0 ? -dy : dy;

      if (adx <= TAP_MAX_DISPLACEMENT && ady <= TAP_MAX_DISPLACEMENT) {
        dispatch(GESTURE_TAP);
        return;
      }
      // Horizontal swipes only — vertical swipes are dropped (drag-scroll
      // owns the vertical axis).
      if (adx >= SWIPE_MIN_DISPLACEMENT &&
          adx * SWIPE_DOMINANCE_DEN >= ady * SWIPE_DOMINANCE_NUM) {
        dispatch(dx > 0 ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT);
        return;
      }
      // Ambiguous — drop.
      break;
    }
  }
}

void touch_input_init(void) {
  for (int i = 0; i < MAX_HANDLERS; i++) {
    s_handlers[i].win = NULL;
    s_handlers[i].handler = NULL;
    s_handlers[i].context = NULL;
  }
  touch_service_subscribe(touch_handler, NULL);
  s_subscribed = true;
}

void touch_input_deinit(void) {
  if (s_subscribed) {
    touch_service_unsubscribe();
    s_subscribed = false;
  }
}

void touch_input_set_handler(Window *win, GestureHandler handler, void *context) {
  if (!win) return;
  HandlerEntry *e = find_entry(win);
  if (!e) {
    for (int i = 0; i < MAX_HANDLERS; i++) {
      if (s_handlers[i].win == NULL) { e = &s_handlers[i]; break; }
    }
  }
  if (!e) return;
  e->win = win;
  e->handler = handler;
  e->context = context;
}

void touch_input_clear_handler(Window *win) {
  HandlerEntry *e = find_entry(win);
  if (e) {
    e->win = NULL;
    e->handler = NULL;
    e->context = NULL;
  }
}

#else  // !HAS_TOUCH — non-touch platforms get no-op stubs

void touch_input_init(void) {}
void touch_input_deinit(void) {}
void touch_input_set_handler(Window *win, GestureHandler handler, void *context) {
  (void)win; (void)handler; (void)context;
}
void touch_input_clear_handler(Window *win) { (void)win; }

#endif
