#include <pebble.h>
#include "more.h"

#if !defined(PBL_PLATFORM_APLITE)

#include "comm.h"
#include "playback_data.h"
#include "touch_input.h"

// Initial state is read from main.c when the window loads; subsequent
// taps toggle a local copy optimistically so the submenu re-renders
// immediately without waiting for the next Spotify poll to confirm.
extern bool main_get_shuffle(void);
extern int  main_get_repeat(void);

static Window *s_window;
static MenuLayer *s_menu;
static bool s_shuffle;
static int  s_repeat;

static const char *shuffle_subtitle(void) {
  return s_shuffle ? "On" : "Off";
}

static const char *repeat_subtitle(void) {
  switch (s_repeat) {
    case 1: return "Context";
    case 2: return "Track";
    default: return "Off";
  }
}

static uint16_t get_num_rows(MenuLayer *m, uint16_t s, void *d) { return 2; }

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *d) {
  if (idx->row == 0) {
    menu_cell_basic_draw(ctx, cell, "Shuffle", shuffle_subtitle(), NULL);
  } else {
    menu_cell_basic_draw(ctx, cell, "Repeat", repeat_subtitle(), NULL);
  }
}

static void select_click(MenuLayer *m, MenuIndex *idx, void *d) {
  if (idx->row == 0) {
    s_shuffle = !s_shuffle;
    comm_send_command(CMD_TOGGLE_SHUFFLE, NULL);
  } else {
    s_repeat = (s_repeat + 1) % 3;
    comm_send_command(CMD_CYCLE_REPEAT, NULL);
  }
  menu_layer_reload_data(s_menu);
}

#if HAS_TOUCH
static void more_gesture(GestureKind kind, void *ctx) {
  if (!s_menu) return;
  switch (kind) {
    case GESTURE_TAP: {
      MenuIndex idx = menu_layer_get_selected_index(s_menu);
      select_click(s_menu, &idx, NULL);
      break;
    }
    case GESTURE_SCROLL_UP:
      // Finger moved UP — push content up — selection advances DOWN.
      menu_layer_set_selected_next(s_menu, false, MenuRowAlignCenter, true);
      break;
    case GESTURE_SCROLL_DOWN:
      // Finger moved DOWN — pull content down — selection retreats UP.
      menu_layer_set_selected_next(s_menu, true, MenuRowAlignCenter, true);
      break;
    case GESTURE_SWIPE_LEFT:
      window_stack_pop(true);
      break;
    case GESTURE_SWIPE_RIGHT:
      // Ignored.
      break;
  }
}
#endif

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_shuffle = main_get_shuffle();
  s_repeat  = main_get_repeat();

  s_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = get_num_rows,
    .draw_row = draw_row,
    .select_click = select_click,
  });
  menu_layer_set_normal_colors(s_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu,
    PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite),
    PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));
#if HAS_TOUCH
  touch_input_set_handler(window, more_gesture, NULL);
#endif
}

static void window_unload(Window *window) {
#if HAS_TOUCH
  touch_input_clear_handler(window);
#endif
  menu_layer_destroy(s_menu);
  s_menu = NULL;
}

void more_window_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

#else  // PBL_PLATFORM_APLITE — submenu not available, but keep symbol for linker
void more_window_push(void) { /* no-op on aplite */ }
#endif
