#pragma once
#include <pebble.h>

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define HAS_TOUCH 1
#else
#define HAS_TOUCH 0
#endif

typedef enum {
  GESTURE_TAP,
  GESTURE_SWIPE_LEFT,
  GESTURE_SWIPE_RIGHT,
  // Drag-scroll: fired during PositionUpdate when the finger crosses
  // ROW_STEP_PX vertically. Naming matches the user's intent — "scroll
  // up the list" reveals later rows (selection moves down), "scroll
  // down" reveals earlier rows (selection moves up). Phone convention.
  GESTURE_SCROLL_UP,
  GESTURE_SCROLL_DOWN,
} GestureKind;

typedef void (*GestureHandler)(GestureKind kind, void *context);

void touch_input_init(void);
void touch_input_deinit(void);

// Per-window: install in window_load, clear in window_unload.
// Dispatch finds the topmost window via window_stack_get_top_window().
void touch_input_set_handler(Window *win, GestureHandler handler, void *context);
void touch_input_clear_handler(Window *win);
