#pragma once
#include <pebble.h>
#include "playback_data.h"

#define QUEUE_TIMEOUT_MS 3000

void list_window_push(ListType type);
void list_add_item(int index, const char *title, const char *subtitle, const char *uri);
void list_set_count(int count);
void list_mark_done(void);
void list_set_status(const char *status);
// Set the current track shown as row 0 in the queue view.
void list_set_queue_current_track(const char *title, const char *artist);
// Auto-dismiss the list window after timeout_ms of inactivity.
void list_set_auto_dismiss(uint32_t timeout_ms);
