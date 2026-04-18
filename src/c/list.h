#pragma once
#include <pebble.h>
#include "playback_data.h"

void list_window_push(ListType type);
void list_add_item(int index, const char *title, const char *subtitle, const char *uri);
void list_set_count(int count);
void list_mark_done(void);
void list_set_status(const char *status);
