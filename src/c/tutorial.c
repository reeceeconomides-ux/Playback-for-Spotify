#include "tutorial.h"

#if defined(PBL_PLATFORM_APLITE)
// Aplite drops the tutorial entirely to free heap (24 KB app slot).
// Provide stubs so the linker is happy; main.c also gates the show-on-launch
// call, and menu.c's About → Tutorial path becomes a no-op.
bool tutorial_needed(void) { return false; }
void tutorial_show(void (*done_cb)(void)) { (void)done_cb; }

#else  // tutorial available on all non-aplite platforms

#include "touch_input.h"

#define TUTORIAL_PERSIST_KEY 42
#define NUM_PAGES 5

static Window *s_window;
static TextLayer *s_title_layer;
static TextLayer *s_body_layer;
static TextLayer *s_hint_layer;
static int s_page = 0;
static void (*s_done_cb)(void);

#if defined(PBL_PLATFORM_APLITE)
static const char *s_titles[] = {
  "Welcome!"
};

static const char *s_bodies[] = {
  "Control Spotify\n"
  "from your wrist.\n\n"
  "UP/DOWN: skip\n"
  "SELECT: queue\n"
  "Hold SELECT:\n"
  "volume / more"
};
#else
static const char *s_titles[] = {
  "Welcome!",
  "Now Playing",
  "Hold UP/DOWN",
  "Shuffle/Repeat",
  "Add to Queue"
};

static const char *s_bodies[] = {
  "Control Spotify\n"
  "from your wrist.\n\n"
  "Connect Spotify\n"
  "in Pebble app\n"
  "Settings.",

  "UP = Previous track\n"
  "DOWN = Next track\n"
  "SELECT = View Queue\n\n"
  "SELECT (in Queue) =\n"
  "Play/Pause track",

  "Hold UP = back 15s\n"
  "Hold DOWN = +15s\n\n"
  "Switch to volume\n"
  "in About menu.",

  "Hold SELECT on\n"
  "'Now Playing' to\n"
  "toggle Shuffle &\n"
  "Repeat mode.",

  "Hold SELECT on\n"
  "a 'Liked Song'\n"
  "to queue it."
};
#endif

static void update_page(void) {
  text_layer_set_text(s_title_layer, s_titles[s_page]);
  text_layer_set_text(s_body_layer, s_bodies[s_page]);

  if (s_page < NUM_PAGES - 1) {
    text_layer_set_text(s_hint_layer, "DOWN for next");
  } else {
    text_layer_set_text(s_hint_layer, "SELECT to start");
  }
}

static void down_handler(ClickRecognizerRef r, void *ctx) {
  if (s_page < NUM_PAGES - 1) {
    s_page++;
    update_page();
  }
}

static void up_handler(ClickRecognizerRef r, void *ctx) {
  if (s_page > 0) {
    s_page--;
    update_page();
  }
}

static void select_handler(ClickRecognizerRef r, void *ctx) {
  if (s_page >= NUM_PAGES - 1) {
    persist_write_bool(TUTORIAL_PERSIST_KEY, true);
    window_stack_pop(true);
    if (s_done_cb) s_done_cb();
  } else {
    s_page++;
    update_page();
  }
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_DOWN, down_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_handler);
}

#if HAS_TOUCH
static void tutorial_gesture(GestureKind kind, void *ctx) {
  switch (kind) {
    case GESTURE_TAP:
      select_handler(NULL, NULL);
      break;
    case GESTURE_SWIPE_LEFT:
      // Right-to-left = next page (matches NP "next track" convention).
      down_handler(NULL, NULL);
      break;
    case GESTURE_SWIPE_RIGHT:
      // Left-to-right = previous page.
      up_handler(NULL, NULL);
      break;
    case GESTURE_SCROLL_UP:
    case GESTURE_SCROLL_DOWN:
      // Tutorial pages are navigated discretely; ignore drag-scroll.
      break;
  }
}
#endif

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  int W = bounds.size.w;
  int H = bounds.size.h;

  s_title_layer = text_layer_create(GRect(4, 4, W - 8, 28));
  text_layer_set_background_color(s_title_layer, GColorClear);
  text_layer_set_text_color(s_title_layer, PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite));
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_title_layer, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
  layer_add_child(root, text_layer_get_layer(s_title_layer));

  s_body_layer = text_layer_create(GRect(4, 34, W - 8, H - 56));
  text_layer_set_background_color(s_body_layer, GColorClear);
  text_layer_set_text_color(s_body_layer, GColorWhite);
  text_layer_set_font(s_body_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_body_layer, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
  layer_add_child(root, text_layer_get_layer(s_body_layer));

  // On round screens, the bottom-right corner falls inside the bezel curve
  // and clips the hint. Move it up and center-align so the whole text fits
  // inside the safe band.
  int hint_y     = PBL_IF_ROUND_ELSE(H - 30, H - 20);
  GTextAlignment hint_align = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentRight);
  s_hint_layer = text_layer_create(GRect(4, hint_y, W - 8, 18));
  text_layer_set_background_color(s_hint_layer, GColorClear);
  text_layer_set_text_color(s_hint_layer, GColorLightGray);
  text_layer_set_font(s_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_hint_layer, hint_align);
  layer_add_child(root, text_layer_get_layer(s_hint_layer));

  window_set_click_config_provider(window, click_config);
#if HAS_TOUCH
  touch_input_set_handler(window, tutorial_gesture, NULL);
#endif
  s_page = 0;
  update_page();
}

static void window_unload(Window *window) {
#if HAS_TOUCH
  touch_input_clear_handler(window);
#endif
  text_layer_destroy(s_hint_layer);
  text_layer_destroy(s_body_layer);
  text_layer_destroy(s_title_layer);
  window_destroy(s_window);
  s_window = NULL;
}

bool tutorial_needed(void) {
  return !persist_read_bool(TUTORIAL_PERSIST_KEY);
}

void tutorial_show(void (*done_cb)(void)) {
  s_done_cb = done_cb;
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

#endif  // !PBL_PLATFORM_APLITE
