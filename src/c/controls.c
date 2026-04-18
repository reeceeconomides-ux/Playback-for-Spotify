#include "controls.h"

#define LONG_PRESS_MS 1000
#define MODE_REVERT_MS 3000

static ControlMode s_mode = CONTROL_MODE_TRACK;
static MusicCommandCallback s_cmd_cb;
static ModeChangedCallback s_mode_cb;
static AppTimer *s_revert_timer = NULL;

static void revert_timer_cb(void *data) {
  s_revert_timer = NULL;
  if (s_mode != CONTROL_MODE_TRACK) {
    s_mode = CONTROL_MODE_TRACK;
    if (s_mode_cb) s_mode_cb(s_mode);
  }
}

static void reset_revert_timer(void) {
  if (s_mode == CONTROL_MODE_VOLUME) {
    if (s_revert_timer) {
      app_timer_reschedule(s_revert_timer, MODE_REVERT_MS);
    } else {
      s_revert_timer = app_timer_register(MODE_REVERT_MS, revert_timer_cb, NULL);
    }
  }
}

static void up_click(ClickRecognizerRef r, void *ctx) {
  if (s_mode == CONTROL_MODE_TRACK) {
    if (s_cmd_cb) s_cmd_cb(MUSIC_CMD_NEXT);
  } else {
    if (s_cmd_cb) s_cmd_cb(MUSIC_CMD_VOL_UP);
    reset_revert_timer();
  }
}

static void down_click(ClickRecognizerRef r, void *ctx) {
  if (s_mode == CONTROL_MODE_TRACK) {
    if (s_cmd_cb) s_cmd_cb(MUSIC_CMD_PREV);
  } else {
    if (s_cmd_cb) s_cmd_cb(MUSIC_CMD_VOL_DOWN);
    reset_revert_timer();
  }
}

static void select_click(ClickRecognizerRef r, void *ctx) {
  if (s_cmd_cb) s_cmd_cb(MUSIC_CMD_PLAY_PAUSE);
  if (s_mode == CONTROL_MODE_VOLUME) {
    reset_revert_timer();
  }
}

static void select_long_click(ClickRecognizerRef r, void *ctx) {
  if (s_mode == CONTROL_MODE_TRACK) {
    s_mode = CONTROL_MODE_VOLUME;
    s_revert_timer = app_timer_register(MODE_REVERT_MS, revert_timer_cb, NULL);
  } else {
    s_mode = CONTROL_MODE_TRACK;
    if (s_revert_timer) {
      app_timer_cancel(s_revert_timer);
      s_revert_timer = NULL;
    }
  }
  vibes_short_pulse();
  if (s_mode_cb) s_mode_cb(s_mode);
}

static void up_long_click(ClickRecognizerRef r, void *ctx) {
  // Volume Mode already uses single-click UP for volume — don't overload
  // long-press with seek there, it'd just fight the mode.
  if (s_mode == CONTROL_MODE_TRACK && s_cmd_cb) {
    s_cmd_cb(MUSIC_CMD_SEEK_FWD);
  }
}

static void down_long_click(ClickRecognizerRef r, void *ctx) {
  if (s_mode == CONTROL_MODE_TRACK && s_cmd_cb) {
    s_cmd_cb(MUSIC_CMD_SEEK_BACK);
  }
}

void controls_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  
  window_long_click_subscribe(BUTTON_ID_SELECT, LONG_PRESS_MS,
                               select_long_click, NULL);
  window_long_click_subscribe(BUTTON_ID_UP, 1000, up_long_click, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 1000, down_long_click, NULL);
}

void controls_init(MusicCommandCallback cmd_cb, ModeChangedCallback mode_cb) {
  s_cmd_cb = cmd_cb;
  s_mode_cb = mode_cb;
  s_mode = CONTROL_MODE_TRACK;
}

void controls_deinit(void) {
  if (s_revert_timer) {
    app_timer_cancel(s_revert_timer);
    s_revert_timer = NULL;
  }
}

ControlMode controls_get_mode(void) {
  return s_mode;
}
