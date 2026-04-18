#pragma once
#include <pebble.h>

typedef enum {
  CONTROL_MODE_TRACK,
  CONTROL_MODE_VOLUME,
} ControlMode;

typedef enum {
  MUSIC_CMD_PLAY_PAUSE,
  MUSIC_CMD_NEXT,
  MUSIC_CMD_PREV,
  MUSIC_CMD_VOL_UP,
  MUSIC_CMD_VOL_DOWN,
  MUSIC_CMD_SEEK_FWD,
  MUSIC_CMD_SEEK_BACK
  } MusicCommand;

typedef void (*MusicCommandCallback)(MusicCommand cmd);
typedef void (*ModeChangedCallback)(ControlMode mode);

void controls_init(MusicCommandCallback cmd_cb, ModeChangedCallback mode_cb);
void controls_deinit(void);
void controls_click_config_provider(void *context);
ControlMode controls_get_mode(void);
