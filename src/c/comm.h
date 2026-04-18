#pragma once
#include <pebble.h>
#include "playback_data.h"

typedef void (*ImageReadyCallback)(GBitmap *bitmap);
typedef void (*StatusCallback)(const char *status);
typedef void (*TrackInfoCallback)(const char *title, const char *artist,
                                  int duration, int elapsed, bool is_playing,
                                  bool shuffle, int repeat_state);
typedef void (*ListItemCallback)(int list_type, int index, int count,
                                 const char *title, const char *subtitle,
                                 const char *uri);
typedef void (*ListDoneCallback)(int list_type);
typedef void (*AuthStatusCallback)(bool authenticated);

typedef struct {
  ImageReadyCallback on_image_ready;
  StatusCallback on_status;
  TrackInfoCallback on_track_info;
  ListItemCallback on_list_item;
  ListDoneCallback on_list_done;
  AuthStatusCallback on_auth_status;
} CommCallbacks;

void comm_init(CommCallbacks callbacks);
void comm_deinit(void);
void comm_send_command(AppCommand cmd, const char *context);
bool comm_is_js_ready(void);
// Returns the most recently received album art bitmap, or NULL if none
// has been loaded yet. Used to repaint art into a freshly recreated
// BitmapLayer when the Now Playing window is reopened.
GBitmap *comm_get_cached_art(void);
