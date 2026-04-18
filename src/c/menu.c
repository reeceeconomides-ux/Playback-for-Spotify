#include "menu.h"
#include "comm.h"
#include "list.h"
#include "playback_data.h"
#include "tutorial.h"
#include "marquee.h"
#include "more.h"

// Forward declaration — implemented in main.c
extern void now_playing_window_push(void);

// --- About window ---
static Window *s_about_window;
static MenuLayer *s_about_menu;

#define APP_VERSION "1.0.0"
#define NUM_ABOUT_ROWS 3

static void tutorial_restart_done(void) {
  // Tutorial finished, return to about/menu
}

static uint16_t about_get_num_rows(MenuLayer *menu, uint16_t s, void *d) {
  return NUM_ABOUT_ROWS;
}

static void about_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *d) {
  switch (idx->row) {
    case 0:
      menu_cell_basic_draw(ctx, cell, "Tutorial", "View controls guide", NULL);
      break;
    case 1:
      menu_cell_basic_draw(ctx, cell, "Author", "alex_pavlov", NULL);
      break;
    case 2:
      menu_cell_basic_draw(ctx, cell, "Version", APP_VERSION, NULL);
      break;
  }
}

static void about_select(MenuLayer *menu, MenuIndex *idx, void *d) {
  if (idx->row == 0) {
    tutorial_show(tutorial_restart_done);
  }
}

static void about_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_about_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_about_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = about_get_num_rows,
    .draw_row = about_draw_row,
    .select_click = about_select,
  });
  menu_layer_set_normal_colors(s_about_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_about_menu,
    PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite),
    PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  menu_layer_set_click_config_onto_window(s_about_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_about_menu));
}

static void about_unload(Window *window) {
  menu_layer_destroy(s_about_menu);
  s_about_menu = NULL;
}

static void about_window_push(void) {
  s_about_window = window_create();
  window_set_background_color(s_about_window, GColorBlack);
  window_set_window_handlers(s_about_window, (WindowHandlers) {
    .load = about_load,
    .unload = about_unload,
  });
  window_stack_push(s_about_window, true);
}

static Window *s_menu_window;
static MenuLayer *s_menu_layer;

static char s_now_playing_subtitle[64] = "Not playing";

static const char *s_titles[] = {
  "Now Playing",
  "Queue",
  "Playlists",
  "Artists",
  "Albums",
  "Liked Songs",
  "Podcasts",
  "About"
};
#define NUM_ROWS 8

// Scroll the "Now Playing" subtitle (track title) while row 0 is
// highlighted. Pixel-based — constant-speed regardless of glyph widths.
static Marquee s_np_subtitle_marquee;
static bool s_np_row_highlighted = false;

#define MENU_ROW_PAD 4

static void np_marquee_redraw(void *ctx) {
  if (s_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
}

static void refresh_np_marquee(void) {
  if (!s_menu_layer || !s_np_row_highlighted) {
    marquee_unregister(&s_np_subtitle_marquee);
    return;
  }
  GRect b = layer_get_bounds(menu_layer_get_layer(s_menu_layer));
  int visible = b.size.w - 2 * MENU_ROW_PAD;
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  marquee_set(&s_np_subtitle_marquee, s_now_playing_subtitle, font, visible,
              np_marquee_redraw, NULL);
}

// --- MenuLayer callbacks ---

static uint16_t get_num_sections(MenuLayer *menu, void *data) {
  return 1;
}

static uint16_t get_num_rows(MenuLayer *menu, uint16_t section, void *data) {
  return NUM_ROWS;
}

static int16_t get_header_height(MenuLayer *menu, uint16_t section, void *data) {
  return 0;
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                     void *data) {
  if (cell_index->row == 0 && s_np_row_highlighted && s_np_subtitle_marquee.overflows) {
    // Draw "Now Playing" title normally, then paint a scrolling
    // subtitle underneath with a pixel offset. menu_cell_basic_draw
    // can't do this — it ellipsizes the subtitle.
    GRect b = layer_get_bounds(cell_layer);
    GFont tfont = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    GRect trect = GRect(MENU_ROW_PAD, -2,
                        b.size.w - 2 * MENU_ROW_PAD, 28);
    graphics_draw_text(ctx, s_titles[0], tfont, trect,
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);

    GFont sfont = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    GRect srect = GRect(MENU_ROW_PAD - s_np_subtitle_marquee.px_offset,
                        24,
                        s_np_subtitle_marquee.text_width + 20, 20);
    graphics_draw_text(ctx, s_np_subtitle_marquee.text, sfont, srect,
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
    return;
  }

  const char *subtitle = (cell_index->row == 0) ? s_now_playing_subtitle : NULL;
  menu_cell_basic_draw(ctx, cell_layer, s_titles[cell_index->row], subtitle, NULL);
}

static void menu_selection_changed(MenuLayer *menu, MenuIndex new_idx,
                                    MenuIndex old_idx, void *data) {
  s_np_row_highlighted = (new_idx.row == 0);
  refresh_np_marquee();
}

static void select_long_callback(MenuLayer *menu, MenuIndex *cell_index, void *data) {
  // Long-press on "Now Playing" opens the Shuffle/Repeat submenu.
  // Other rows ignore the long-press so it can't accidentally push a
  // surprising window from a list row.
  if (cell_index->row == 0) {
    more_window_push();
  }
}

static void select_callback(MenuLayer *menu, MenuIndex *cell_index, void *data) {
  switch (cell_index->row) {
    case 0:
      now_playing_window_push();
      comm_send_command(CMD_FETCH_NOW_PLAYING, NULL);
      break;
    case 1:
      list_window_push(LIST_TYPE_QUEUE);
      comm_send_command(CMD_FETCH_QUEUE, NULL);
      break;
    case 2:
      list_window_push(LIST_TYPE_PLAYLISTS);
      comm_send_command(CMD_FETCH_PLAYLISTS, NULL);
      break;
    case 3:
      list_window_push(LIST_TYPE_ARTISTS);
      comm_send_command(CMD_FETCH_ARTISTS, NULL);
      break;
    case 4:
      list_window_push(LIST_TYPE_ALBUMS);
      comm_send_command(CMD_FETCH_ALBUMS, NULL);
      break;
    case 5:
      list_window_push(LIST_TYPE_LIKED_SONGS);
      comm_send_command(CMD_FETCH_LIKED_SONGS, NULL);
      break;
    case 6:
      list_window_push(LIST_TYPE_SHOWS);
      comm_send_command(CMD_FETCH_SHOWS, NULL);
      break;
    case 7:
      about_window_push();
      break;
  }
}

// --- Window handlers ---

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_sections = get_num_sections,
    .get_num_rows = get_num_rows,
    .get_header_height = get_header_height,
    .draw_row = draw_row,
    .select_click = select_callback,
    .select_long_click = select_long_callback,
    .selection_changed = menu_selection_changed,
  });

  menu_layer_set_normal_colors(s_menu_layer, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu_layer,
    PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite),
    PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  menu_layer_set_click_config_onto_window(s_menu_layer, window);

  layer_add_child(root, menu_layer_get_layer(s_menu_layer));

  s_np_row_highlighted = true;  // row 0 is selected on load
  refresh_np_marquee();
}

static void window_unload(Window *window) {
  marquee_unregister(&s_np_subtitle_marquee);
  s_np_row_highlighted = false;
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
}

// --- Public API ---

void menu_window_push(void) {
  s_menu_window = window_create();
  window_set_background_color(s_menu_window, GColorBlack);
  window_set_window_handlers(s_menu_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_menu_window, true);
}

void menu_set_now_playing_subtitle(const char *subtitle) {
  strncpy(s_now_playing_subtitle, subtitle, sizeof(s_now_playing_subtitle) - 1);
  s_now_playing_subtitle[sizeof(s_now_playing_subtitle) - 1] = '\0';
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
    refresh_np_marquee();
  }
}
