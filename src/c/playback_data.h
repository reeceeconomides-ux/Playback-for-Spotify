#pragma once
#include <pebble.h>

// Commands sent from watch to JS (via Command message key)
typedef enum {
  CMD_FETCH_NOW_PLAYING  = 1,
  CMD_FETCH_PLAYLISTS    = 2,
  CMD_FETCH_ARTISTS      = 3,
  CMD_FETCH_ALBUMS       = 4,
  CMD_FETCH_LIKED_SONGS  = 5,
  CMD_FETCH_SHOWS        = 6,
  CMD_FETCH_QUEUE        = 8,
  CMD_PLAY_PAUSE         = 10,
  CMD_NEXT_TRACK         = 11,
  CMD_PREV_TRACK         = 12,
  CMD_VOLUME_UP          = 13,
  CMD_VOLUME_DOWN        = 14,
  CMD_SEEK_FORWARD       = 17,
  CMD_SEEK_BACK          = 18,
  CMD_PLAY_CONTEXT       = 20,
  CMD_PLAY_SHOW          = 21,
  CMD_TOGGLE_SHUFFLE     = 24,
  CMD_CYCLE_REPEAT       = 25,
  CMD_QUEUE_ADD          = 26,
  CMD_QUEUE_SKIP_TO      = 27, // skip forward N tracks to reach a queue position
  CMD_FETCH_ART          = 30,
} AppCommand;

// Long-press behavior on Now Playing (UP/DOWN held).
// In LP_MODE_SEEK:   UP = back 15s, DOWN = forward 15s.
// In LP_MODE_VOLUME: UP = volume +,  DOWN = volume -.
// Persisted across launches; user toggles in About menu.
typedef enum {
  LP_MODE_SEEK   = 0,
  LP_MODE_VOLUME = 1
} LongPressMode;

LongPressMode long_press_mode_get(void);
void long_press_mode_toggle(void);

// List types
typedef enum {
  LIST_TYPE_PLAYLISTS    = 0,
  LIST_TYPE_ARTISTS      = 1,
  LIST_TYPE_ALBUMS       = 2,
  LIST_TYPE_LIKED_SONGS  = 3,
  LIST_TYPE_SHOWS        = 4,
  LIST_TYPE_QUEUE        = 5
} ListType;

#if defined(PBL_PLATFORM_APLITE)
// Aplite has only 24 KB total app slot — shrink list buffers to fit.
// Cost: ListItem 152 → 112 B, MAX_LIST_ITEMS 20 → 8.
// s_items[] drops from 3040 B to 896 B (saves 2144 B bss).
#define MAX_LIST_ITEMS 8

typedef struct {
  char title[32];
  char subtitle[24];
  char uri[56];
} ListItem;
#else
#define MAX_LIST_ITEMS 20

typedef struct {
  char title[40];
  char subtitle[32];
  char uri[80];
} ListItem;
#endif
