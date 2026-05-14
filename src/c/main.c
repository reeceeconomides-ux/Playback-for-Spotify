#include <pebble.h>
#include "comm.h"
#include "controls.h"
#include "ui.h"
#include "menu.h"
#include "list.h"
#include "playback_data.h"
#include "tutorial.h"
#include "touch_input.h"

static Window *s_np_window;
static bool s_np_visible = false;

// Playback state (synced from Spotify, interpolated locally)
static bool s_playing = false;
static int s_elapsed = 0;
static int s_total = 0;
static bool s_shuffle = false;
static int s_repeat_state = 0; // 0=off, 1=context, 2=track
static AppTimer *s_tick_timer = NULL;

// Current track info (cached to populate queue row 0)
static char s_current_title[40] = "";
static char s_current_artist[32] = "";

// Exposed for the More submenu (more.c) so it can show the current
// shuffle/repeat state on open without re-fetching.
bool main_get_shuffle(void) { return s_shuffle; }
int  main_get_repeat(void)  { return s_repeat_state; }

// --- Long-press mode (persisted; user-selectable in About) ---
// Tutorial uses key 42; pick 43 to avoid collision.
#define LONG_PRESS_PERSIST_KEY 43

static LongPressMode s_long_press_mode = LP_MODE_SEEK;

static void long_press_mode_load(void) {
  s_long_press_mode = (LongPressMode)persist_read_int(LONG_PRESS_PERSIST_KEY);
}

LongPressMode long_press_mode_get(void) {
  return s_long_press_mode;
}

void long_press_mode_toggle(void) {
  s_long_press_mode = (s_long_press_mode == LP_MODE_SEEK) ? LP_MODE_VOLUME : LP_MODE_SEEK;
  persist_write_int(LONG_PRESS_PERSIST_KEY, (int)s_long_press_mode);
}

// --- Progress timer (local interpolation between Spotify polls) ---

static void tick_cb(void *data) {
  s_tick_timer = NULL;
  if (s_playing && s_elapsed < s_total) {
    s_elapsed++;
    ui_set_progress(s_elapsed, s_total);
    s_tick_timer = app_timer_register(1000, tick_cb, NULL);
  }
}

static void start_ticking(void) {
  if (!s_tick_timer && s_playing) {
    s_tick_timer = app_timer_register(1000, tick_cb, NULL);
  }
}

static void stop_ticking(void) {
  if (s_tick_timer) {
    app_timer_cancel(s_tick_timer);
    s_tick_timer = NULL;
  }
}

// --- Comm callbacks ---

static void image_ready_cb(GBitmap *bitmap) {
  ui_set_album_art(bitmap);
}

static void status_cb(const char *status) {
  ui_set_status(status);
  list_set_status(status);
  if (strcmp(status, "Added to queue") == 0) {
    vibes_short_pulse();
  }
}

static void track_info_cb(const char *title, const char *artist,
                           int duration, int elapsed, bool is_playing,
                           bool shuffle, int repeat_state) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "track_info_cb enter");

  // Update playback state
  s_total = duration;
  s_elapsed = elapsed;
  s_playing = is_playing;
  s_shuffle = shuffle;
  s_repeat_state = repeat_state;

  strncpy(s_current_title, title, sizeof(s_current_title) - 1);
  s_current_title[sizeof(s_current_title) - 1] = '\0';
  strncpy(s_current_artist, artist, sizeof(s_current_artist) - 1);
  s_current_artist[sizeof(s_current_artist) - 1] = '\0';

  // Update Now Playing UI (no-ops if window not showing)
  APP_LOG(APP_LOG_LEVEL_DEBUG, "track_info_cb: ui update");
  ui_set_track_info(title, artist);
  ui_set_progress(elapsed, duration);
  ui_set_shuffle(shuffle);
  ui_set_repeat(repeat_state);

  // Update menu subtitle
  APP_LOG(APP_LOG_LEVEL_DEBUG, "track_info_cb: menu subtitle");
  menu_set_now_playing_subtitle(title);

  // Only tick when Now Playing window is visible
  APP_LOG(APP_LOG_LEVEL_DEBUG, "track_info_cb: timer");
  stop_ticking();
  if (s_playing && s_np_visible) {
    start_ticking();
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG, "track_info_cb done");
}

static void list_item_cb(int list_type, int index, int count,
                          const char *title, const char *subtitle,
                          const char *uri) {
  list_set_count(count);
  list_add_item(index, title, subtitle, uri);
}

static void list_done_cb(int list_type) {
  list_mark_done();
}

static void auth_status_cb(bool authenticated) {
  if (authenticated) {
    ui_set_status("Spotify connected");
    // Fetch current playback on auth
    comm_send_command(CMD_FETCH_NOW_PLAYING, NULL);
  } else {
    ui_set_status("Open Settings to login");
  }
}

// --- Controls callbacks ---

static void music_command_cb(MusicCommand cmd) {
  switch (cmd) {
    case MUSIC_CMD_PLAY_PAUSE:
      comm_send_command(CMD_PLAY_PAUSE, NULL);
      ui_set_status(s_playing ? "Pausing..." : "Playing...");
      break;
    case MUSIC_CMD_NEXT:
      ui_hint_animation_dir(false);
      comm_send_command(CMD_NEXT_TRACK, NULL);
      ui_set_status("Next...");
      break;
    case MUSIC_CMD_PREV:
      ui_hint_animation_dir(true);
      comm_send_command(CMD_PREV_TRACK, NULL);
      ui_set_status("Previous...");
      break;
    case MUSIC_CMD_VOL_UP:
      comm_send_command(CMD_VOLUME_UP, NULL);
      ui_set_status("Volume +");
      break;
    case MUSIC_CMD_VOL_DOWN:
      comm_send_command(CMD_VOLUME_DOWN, NULL);
      ui_set_status("Volume -");
      break;
    case MUSIC_CMD_SEEK_FWD:
      comm_send_command(CMD_SEEK_FORWARD, NULL);
      ui_set_status("Forward 15s");
      // Optimistic UI: shift progress now, the next poll reconciles.
      if (s_total > 0) {
        s_elapsed += 15;
        if (s_elapsed > s_total) s_elapsed = s_total;
        ui_set_progress(s_elapsed, s_total);
      }
      break;
    case MUSIC_CMD_SEEK_BACK:
      comm_send_command(CMD_SEEK_BACK, NULL);
      ui_set_status("Back 15s");
      if (s_total > 0) {
        s_elapsed -= 15;
        if (s_elapsed < 0) s_elapsed = 0;
        ui_set_progress(s_elapsed, s_total);
      }
      break;
  }
}

static void mode_changed_cb(ControlMode mode) {
  if (mode == CONTROL_MODE_VOLUME) {
    ui_set_status("Volume Mode");
  } else {
    ui_set_status("Track Mode");
  }
}

// --- Now Playing window click config ---

static void np_up_click(ClickRecognizerRef r, void *ctx) {
  ui_hint_animation_dir(true);   // reverse: exit right, enter from left
  comm_send_command(CMD_PREV_TRACK, NULL);
}

static void np_down_click(ClickRecognizerRef r, void *ctx) {
  ui_hint_animation_dir(false);  // forward: exit left, enter from right
  comm_send_command(CMD_NEXT_TRACK, NULL);
}

static void np_select_click(ClickRecognizerRef r, void *ctx) {
  list_set_queue_current_track(s_current_title, s_current_artist);
  list_window_push(LIST_TYPE_QUEUE);
  comm_send_command(CMD_FETCH_QUEUE, NULL);
  list_set_auto_dismiss(QUEUE_TIMEOUT_MS);
}

#if !defined(PBL_PLATFORM_APLITE)
// Hold UP/DOWN on NP: jumps in the scrubber direction (UP = earlier, DOWN
// = later) when in SEEK mode; in VOLUME mode UP/DOWN follow the usual
// louder/quieter convention. music_command_cb handles status text and
// optimistic UI updates.
static void np_up_long_click(ClickRecognizerRef r, void *ctx) {
  music_command_cb(s_long_press_mode == LP_MODE_VOLUME ? MUSIC_CMD_VOL_UP : MUSIC_CMD_SEEK_BACK);
}

static void np_down_long_click(ClickRecognizerRef r, void *ctx) {
  music_command_cb(s_long_press_mode == LP_MODE_VOLUME ? MUSIC_CMD_VOL_DOWN : MUSIC_CMD_SEEK_FWD);
}
#endif

static void np_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, np_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, np_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, np_select_click);
#if !defined(PBL_PLATFORM_APLITE)
  window_long_click_subscribe(BUTTON_ID_UP,   700, np_up_long_click,   NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 700, np_down_long_click, NULL);
#endif
}

#if HAS_TOUCH
static void np_gesture(GestureKind kind, void *ctx) {
  switch (kind) {
    case GESTURE_TAP:
      np_select_click(NULL, NULL);
      break;
    case GESTURE_SWIPE_LEFT:
      // Right-to-left = next track (matches phone music apps).
      np_down_click(NULL, NULL);
      break;
    case GESTURE_SWIPE_RIGHT:
      // Left-to-right = previous track.
      np_up_click(NULL, NULL);
      break;
    case GESTURE_SCROLL_UP:
    case GESTURE_SCROLL_DOWN:
      // Nothing to scroll on Now Playing — ignore.
      break;
  }
}
#endif

// --- Now Playing window (pushed from menu) ---

static void np_window_load(Window *window) {
  s_np_visible = true;
  ui_init(window);
  window_set_click_config_provider(window, np_click_config_provider);
#if HAS_TOUCH
  touch_input_set_handler(window, np_gesture, NULL);
#endif
  GBitmap *cached = comm_get_cached_art();
  if (cached) {
    ui_set_album_art(cached);
  }
}

static void np_window_unload(Window *window) {
  s_np_visible = false;
  stop_ticking();
#if HAS_TOUCH
  touch_input_clear_handler(window);
#endif
  ui_deinit();
#if defined(PBL_PLATFORM_APLITE)
  // Aplite has only ~4 KB free heap; the 1.2 KB album art bitmap blocks
  // list windows from allocating their MenuLayer. Drop the cache when we
  // leave NP — JS will re-fetch on the next poll if the user comes back.
  comm_free_all_art();
#endif
}

void now_playing_window_push(void) {
  s_np_window = window_create();
  window_set_background_color(s_np_window, GColorBlack);
  window_set_window_handlers(s_np_window, (WindowHandlers) {
    .load = np_window_load,
    .unload = np_window_unload,
  });
  window_stack_push(s_np_window, true);
}

// --- Tutorial callback ---

static void tutorial_done(void) {
  // Tutorial finished, now show the menu
}

// --- App lifecycle ---

static void init(void) {
  CommCallbacks cbs = {
    .on_image_ready = image_ready_cb,
    .on_status = status_cb,
    .on_track_info = track_info_cb,
    .on_list_item = list_item_cb,
    .on_list_done = list_done_cb,
    .on_auth_status = auth_status_cb,
  };
  comm_init(cbs);
  controls_init(music_command_cb, mode_changed_cb);
  touch_input_init();
  long_press_mode_load();
  menu_window_push();

  if (launch_reason() == APP_LAUNCH_QUICK_LAUNCH) {
    now_playing_window_push();
    ui_start_loading();
  }

#if !defined(PBL_PLATFORM_APLITE)
  // Tutorial is dropped on aplite to free heap (24 KB app slot).
  // Aplite users see the menu directly on first launch.
  if (tutorial_needed()) {
    tutorial_show(tutorial_done);
  }
#endif
}

static void deinit(void) {
  stop_ticking();
  controls_deinit();
  touch_input_deinit();
  comm_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
