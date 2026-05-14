#include "list.h"
#include "comm.h"
#include "marquee.h"
#include "touch_input.h"
#include "ui.h"

// Forward declaration — implemented in main.c
extern void now_playing_window_push(void);

static Window *s_list_window;
static MenuLayer *s_list_menu;

static ListItem s_items[MAX_LIST_ITEMS];
static int s_item_count = 0;
static int s_items_received = 0;
static bool s_loading = true;
static ListType s_current_type;

// Queue: the currently-playing track shown as the pinned row 0.
static char s_queue_current_title[40] = "";
static char s_queue_current_artist[32] = "";

// Pre-fetched queue data is ready to display without a loading delay.
static bool s_queue_staged = false;

// Auto-dismiss timer (used when the queue is opened from Now Playing).
static AppTimer *s_dismiss_timer = NULL;
static uint32_t s_dismiss_ms = 0;

// The single "active" row marquee. Tracks whichever row the user is
// currently looking at — the highlighted item, or the synthetic empty-
// state row ("No podcasts followed" etc.) when the list is empty.
//
// Scrolling is pixel-based: every tick we bump px_offset by a fixed
// number of pixels and repaint. Char-based shifting was jittery with
// variable-width fonts because each glyph moved a different distance.
static Marquee s_row_marquee;
static int s_marquee_row = -1;

#define LIST_ROW_PAD        4
#define LIST_TITLE_Y        (-2)
#define LIST_TITLE_H        28

static const char *empty_text_for_type(ListType type);

static void row_marquee_redraw(void *ctx) {
  if (s_list_menu) layer_mark_dirty(menu_layer_get_layer(s_list_menu));
}

// Resolve which text the currently-displayed row should show. Returns
// NULL if nothing to marquee (loading state).
static const char *current_row_text(int *out_row) {
  if (s_current_type == LIST_TYPE_QUEUE) {
    int row = 0;
    if (s_list_menu) row = menu_layer_get_selected_index(s_list_menu).row;
    if (out_row) *out_row = row;
    if (row == 0) return s_queue_current_title[0] ? s_queue_current_title : NULL;
    int item_row = row - 1;
    if (item_row >= s_items_received) return NULL;
    return s_items[item_row].title;
  }
  if (s_loading && s_items_received == 0) return NULL;
  if (s_items_received == 0) {
    if (out_row) *out_row = 0;
    return empty_text_for_type(s_current_type);
  }
  int row = 0;
  if (s_list_menu) row = menu_layer_get_selected_index(s_list_menu).row;
  if (row < 0 || row >= s_items_received) return NULL;
  if (out_row) *out_row = row;
  return s_items[row].title;
}

static void refresh_row_marquee(void) {
#if defined(PBL_PLATFORM_APLITE)
  // Aplite: skip marquee. Long titles ellipsize via menu_cell_basic_draw fallback
  // (overflows stays false → draw_row uses the plain path).
  marquee_unregister(&s_row_marquee);
  s_marquee_row = -1;
#else
  int row = -1;
  const char *text = current_row_text(&row);
  if (!text || !s_list_menu) {
    marquee_unregister(&s_row_marquee);
    s_marquee_row = -1;
    return;
  }
  s_marquee_row = row;
  GRect b = layer_get_bounds(menu_layer_get_layer(s_list_menu));
  int visible = b.size.w - 2 * LIST_ROW_PAD;
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  marquee_set(&s_row_marquee, text, font, visible, row_marquee_redraw, NULL);
#endif
}

// Draw a scrolled title (and optional subtitle) manually, bypassing
// menu_cell_basic_draw so we can apply the pixel offset. MenuLayer has
// already filled the cell with the correct normal/highlight bg color
// and set the foreground text color before calling draw_row.
static void draw_scrolling_row(GContext *ctx, const Layer *cell_layer,
                                const char *subtitle) {
  GRect bounds = layer_get_bounds(cell_layer);
  GFont tfont = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);

  // Without a subtitle, center the title vertically — otherwise it hugs
  // the top of the cell (LIST_TITLE_Y = -2) because that y is meant to
  // leave room for the subtitle below.
  bool has_sub = subtitle && subtitle[0];
  int title_y = has_sub ? LIST_TITLE_Y
                        : (bounds.size.h - LIST_TITLE_H) / 2 - 2;
  GRect trect = GRect(LIST_ROW_PAD - s_row_marquee.px_offset,
                      title_y,
                      s_row_marquee.text_width + 20,
                      LIST_TITLE_H);
  graphics_draw_text(ctx, s_row_marquee.text, tfont, trect,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  if (has_sub) {
    GFont sfont = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    GRect srect = GRect(LIST_ROW_PAD, LIST_TITLE_H - 4,
                        bounds.size.w - 2 * LIST_ROW_PAD, 20);
    graphics_draw_text(ctx, subtitle, sfont, srect,
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
  }
}

static const char *s_type_titles[] = {
  "Playlists", "Artists", "Albums", "Liked Songs", "Podcasts",
  "Queue"
};

static char s_status_buf_list[64];
static bool s_showing_list_status = false;
static AppTimer *s_list_status_timer = NULL;

static void list_status_clear_cb(void *data) {
  s_list_status_timer = NULL;
  s_showing_list_status = false;
  if (s_list_menu) layer_mark_dirty(menu_layer_get_layer(s_list_menu));
}

static void dismiss_timer_cb(void *data) {
  s_dismiss_timer = NULL;
  if (s_list_window) window_stack_remove(s_list_window, true);
}

static void reset_dismiss_timer(void) {
  if (!s_dismiss_ms) return;
  if (s_dismiss_timer) {
    app_timer_reschedule(s_dismiss_timer, s_dismiss_ms);
  } else {
    s_dismiss_timer = app_timer_register(s_dismiss_ms, dismiss_timer_cb, NULL);
  }
}

// --- MenuLayer callbacks ---

static uint16_t get_num_sections(MenuLayer *menu, void *data) {
  return 1;
}

static uint16_t get_num_rows(MenuLayer *menu, uint16_t section, void *data) {
  if (s_current_type == LIST_TYPE_QUEUE) {
    return s_items_received + 1; // row 0 is always the current track
  }
  if (s_loading && s_items_received == 0) return 1; // "Loading..." row
  if (!s_loading && s_items_received == 0) return 1; // Empty-state row
  return s_items_received;
}

static const char *empty_text_for_type(ListType type) {
  switch (type) {
    case LIST_TYPE_PLAYLISTS:   return "No playlists";
    case LIST_TYPE_ARTISTS:     return "No artists followed";
    case LIST_TYPE_ALBUMS:      return "No saved albums";
    case LIST_TYPE_LIKED_SONGS: return "No liked songs";
    case LIST_TYPE_SHOWS:       return "No podcasts followed";
    case LIST_TYPE_QUEUE:       return "Queue is empty";
  }
  return "Empty";
}

static int16_t get_header_height(MenuLayer *menu, uint16_t section, void *data) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section,
                         void *data) {
  const char *title = s_showing_list_status ? s_status_buf_list : s_type_titles[s_current_type];
  menu_cell_basic_header_draw(ctx, cell_layer, title);
}


static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                     void *data) {
  if (s_current_type == LIST_TYPE_QUEUE) {
    int row = cell_index->row;
    if (row == 0) {
      const char *sub = s_queue_current_artist[0] ? s_queue_current_artist : NULL;
      const char *title = s_queue_current_title[0] ? s_queue_current_title : "Now Playing";
      menu_cell_basic_draw(ctx, cell_layer, title, sub, NULL);
      return;
    }
    int item_row = row - 1;
    if (item_row >= s_items_received) {
      menu_cell_basic_draw(ctx, cell_layer, "Loading...", NULL, NULL);
      return;
    }
    const char *subtitle = s_items[item_row].subtitle[0] ? s_items[item_row].subtitle : NULL;
    menu_cell_basic_draw(ctx, cell_layer, s_items[item_row].title, subtitle, NULL);
    return;
  }

  if (s_loading && s_items_received == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Loading...", NULL, NULL);
    return;
  }

  if (s_items_received == 0) {
    // Empty-state row. Scroll if it doesn't fit.
    if (s_row_marquee.overflows) {
      draw_scrolling_row(ctx, cell_layer, NULL);
    } else {
      menu_cell_basic_draw(ctx, cell_layer,
                           empty_text_for_type(s_current_type), NULL, NULL);
    }
    return;
  }

  int row = cell_index->row;
  if (row >= s_items_received) return;

  const char *subtitle = s_items[row].subtitle[0] ? s_items[row].subtitle : NULL;
  if (row == s_marquee_row && s_row_marquee.overflows) {
    draw_scrolling_row(ctx, cell_layer, subtitle);
  } else {
    menu_cell_basic_draw(ctx, cell_layer, s_items[row].title, subtitle, NULL);
  }
}

static void selection_changed(MenuLayer *menu, MenuIndex new_idx,
                               MenuIndex old_idx, void *data) {
  refresh_row_marquee();
  reset_dismiss_timer();
}

static void select_long_callback(MenuLayer *menu, MenuIndex *cell_index, void *data) {
  // Add-to-queue on long-press. Only supported for individual tracks
  // (Liked Songs) to avoid Spotify API 403 errors when queuing collections.
  if (s_items_received == 0) return;
  if (s_current_type != LIST_TYPE_LIKED_SONGS) {
    list_set_status("Cannot queue list");
    return;
  }
  int row = cell_index->row;
  if (row >= s_items_received) return;
  comm_send_command(CMD_QUEUE_ADD, s_items[row].uri);
}

static void select_callback(MenuLayer *menu, MenuIndex *cell_index, void *data) {
  reset_dismiss_timer();

  if (s_current_type == LIST_TYPE_QUEUE) {
    if (cell_index->row == 0) {
      comm_send_command(CMD_PLAY_PAUSE, NULL);
    } else {
      int item_row = cell_index->row - 1;
      if (item_row >= s_items_received) return;
      // Skipping forward in the queue — animate exit-left, enter-right.
      ui_hint_animation_dir(false);
      // Send the target URI; pkjs picks context+offset (preserves
      // playlist/album context) or falls back to a uris[] tail.
      comm_send_command(CMD_QUEUE_SKIP_TO, s_items[item_row].uri);
    }
    window_stack_remove(s_list_window, true);
    return;
  }

  if (s_items_received == 0) return; // "Loading..." or empty-state row

  int row = cell_index->row;
  if (row >= s_items_received) return;

  AppCommand play_cmd;
  bool push_np = true;
  switch (s_current_type) {
    case LIST_TYPE_SHOWS:
      // Shows can't be played via /me/player/play with a context_uri —
      // Spotify rejects show URIs there. JS resolves the show's latest
      // episode and plays that via a uris payload instead.
      play_cmd = CMD_PLAY_SHOW;
      break;
    default:
      play_cmd = CMD_PLAY_CONTEXT;
      break;
  }
  comm_send_command(play_cmd, s_items[row].uri);
  if (push_np) now_playing_window_push();
}

// --- Gestures ---

#if HAS_TOUCH
static void list_gesture(GestureKind kind, void *ctx) {
  if (!s_list_menu) return;
  reset_dismiss_timer();
  switch (kind) {
    case GESTURE_TAP: {
      MenuIndex idx = menu_layer_get_selected_index(s_list_menu);
      select_callback(s_list_menu, &idx, NULL);
      break;
    }
    case GESTURE_SCROLL_UP:
      // Finger moved UP — push content up — selection advances DOWN.
      menu_layer_set_selected_next(s_list_menu, false, MenuRowAlignCenter, true);
      break;
    case GESTURE_SCROLL_DOWN:
      // Finger moved DOWN — pull content down — selection retreats UP.
      menu_layer_set_selected_next(s_list_menu, true, MenuRowAlignCenter, true);
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

// --- Window handlers ---

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_list_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_list_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = get_num_sections,
    .get_num_rows = get_num_rows,
    .get_header_height = get_header_height,
    .draw_header = draw_header,
    .draw_row = draw_row,
    .select_click = select_callback,
    .select_long_click = select_long_callback,
    .selection_changed = selection_changed,
  });

  menu_layer_set_normal_colors(s_list_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_list_menu,
    PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite),
    PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  menu_layer_set_click_config_onto_window(s_list_menu, window);

  layer_add_child(root, menu_layer_get_layer(s_list_menu));
  // Initialize marquee for whatever row is selected at open time.
  // Without this, draw_scrolling_row uses an uninitialized text_width
  // until the first external event (e.g. an incoming list item) triggers
  // refresh_row_marquee(), causing a visible position jump.
  refresh_row_marquee();

#if HAS_TOUCH
  touch_input_set_handler(window, list_gesture, NULL);
#endif
}

static void window_unload(Window *window) {
#if HAS_TOUCH
  touch_input_clear_handler(window);
#endif
  if (s_dismiss_timer) {
    app_timer_cancel(s_dismiss_timer);
    s_dismiss_timer = NULL;
  }
  s_dismiss_ms = 0;
  marquee_unregister(&s_row_marquee);
  s_marquee_row = -1;
  menu_layer_destroy(s_list_menu);
  s_list_menu = NULL;
}

// --- Public API ---

void list_window_push(ListType type) {
  s_current_type = type;
  // Use staged data for queue if available (pre-fetched after art loaded).
  if (!(type == LIST_TYPE_QUEUE && s_queue_staged)) {
    s_item_count = 0;
    s_items_received = 0;
    s_loading = true;
  }
  s_queue_staged = false;

  s_list_window = window_create();
  window_set_background_color(s_list_window, GColorBlack);
  window_set_window_handlers(s_list_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_list_window, type != LIST_TYPE_QUEUE);
}

void list_add_item(int index, const char *title, const char *subtitle,
                   const char *uri) {
  if (index >= MAX_LIST_ITEMS) return;

  strncpy(s_items[index].title, title, sizeof(s_items[index].title) - 1);
  s_items[index].title[sizeof(s_items[index].title) - 1] = '\0';

  strncpy(s_items[index].subtitle, subtitle, sizeof(s_items[index].subtitle) - 1);
  s_items[index].subtitle[sizeof(s_items[index].subtitle) - 1] = '\0';

  strncpy(s_items[index].uri, uri, sizeof(s_items[index].uri) - 1);
  s_items[index].uri[sizeof(s_items[index].uri) - 1] = '\0';

  if (index >= s_items_received) {
    s_items_received = index + 1;
  }

  if (s_list_menu) {
    menu_layer_reload_data(s_list_menu);
    // For non-queue lists, kick the marquee when item 0 arrives since
    // that's the initially-selected row. For the queue, row 0 is the
    // pinned current track (not s_items[0]) and the marquee was already
    // initialized at window_load, so skip to avoid a spurious re-measure.
    if (index == 0 && s_current_type != LIST_TYPE_QUEUE) refresh_row_marquee();
  }
}

void list_set_count(int count) {
  s_item_count = (count > MAX_LIST_ITEMS) ? MAX_LIST_ITEMS : count;
}

void list_mark_done(void) {
  s_loading = false;
  if (s_current_type == LIST_TYPE_QUEUE && !s_list_menu) {
    s_queue_staged = true;
  }
  if (s_list_menu) {
    menu_layer_reload_data(s_list_menu);
    // If the list came back empty, this registers a marquee on the
    // empty-state text; otherwise it's a no-op refresh.
    refresh_row_marquee();
  }
}

void list_set_status(const char *status) {
  if (!s_list_menu) return;
  strncpy(s_status_buf_list, status, sizeof(s_status_buf_list) - 1);
  s_status_buf_list[sizeof(s_status_buf_list) - 1] = '\0';
  s_showing_list_status = true;
  layer_mark_dirty(menu_layer_get_layer(s_list_menu));

  if (s_list_status_timer) app_timer_cancel(s_list_status_timer);
  s_list_status_timer = app_timer_register(2000, list_status_clear_cb, NULL);
}

void list_set_queue_current_track(const char *title, const char *artist) {
  strncpy(s_queue_current_title, title, sizeof(s_queue_current_title) - 1);
  s_queue_current_title[sizeof(s_queue_current_title) - 1] = '\0';
  strncpy(s_queue_current_artist, artist, sizeof(s_queue_current_artist) - 1);
  s_queue_current_artist[sizeof(s_queue_current_artist) - 1] = '\0';
}

void list_set_auto_dismiss(uint32_t timeout_ms) {
  s_dismiss_ms = timeout_ms;
  reset_dismiss_timer();
}
