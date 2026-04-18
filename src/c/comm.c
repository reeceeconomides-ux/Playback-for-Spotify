#include "comm.h"

static CommCallbacks s_callbacks;

static GBitmap *s_art_bitmap = NULL;
static uint8_t *s_art_data = NULL;
static uint32_t s_image_data_size = 0;
static uint16_t s_total_chunks = 0;
static uint16_t s_received_chunks = 0;
static uint32_t s_bytes_received = 0;

// JS ready handshake
static bool s_js_ready = false;
static bool s_has_queued_command = false;
static AppCommand s_queued_cmd;
static char s_queued_context[128];

static char s_status_buf[64];

static void send_command_msg(AppCommand cmd, const char *context);

static void handle_js_ready(void) {
  s_js_ready = true;
  APP_LOG(APP_LOG_LEVEL_INFO, "JS ready received");
  if (s_callbacks.on_status) s_callbacks.on_status("Phone connected");

  if (s_has_queued_command) {
    s_has_queued_command = false;
    send_command_msg(s_queued_cmd, s_queued_context[0] ? s_queued_context : NULL);
  }
}

static void handle_error_msg(DictionaryIterator *iter) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_ErrorMsg);
  if (t) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "JS error: %s", t->value->cstring);
    snprintf(s_status_buf, sizeof(s_status_buf), "Error: %s", t->value->cstring);
    if (s_callbacks.on_status) s_callbacks.on_status(s_status_buf);
  }
}

static void handle_status_msg(DictionaryIterator *iter) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_StatusMsg);
  if (t) {
    APP_LOG(APP_LOG_LEVEL_INFO, "JS status: %s", t->value->cstring);
    if (s_callbacks.on_status) s_callbacks.on_status(t->value->cstring);
  }
}

static void handle_image_header(DictionaryIterator *iter) {
  Tuple *w_tuple = dict_find(iter, MESSAGE_KEY_ImageWidth);
  Tuple *h_tuple = dict_find(iter, MESSAGE_KEY_ImageHeight);
  Tuple *size_tuple = dict_find(iter, MESSAGE_KEY_ImageDataSize);
  Tuple *chunks_tuple = dict_find(iter, MESSAGE_KEY_ImageChunksTotal);

  if (!w_tuple || !h_tuple || !size_tuple || !chunks_tuple) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Incomplete image header");
    return;
  }

  uint16_t width = w_tuple->value->uint16;
  uint16_t height = h_tuple->value->uint16;
  uint32_t data_size = size_tuple->value->uint32;
  uint16_t total_chunks = chunks_tuple->value->uint16;

  APP_LOG(APP_LOG_LEVEL_INFO, "Image header: %dx%d, %lu bytes, %d chunks",
          width, height, (unsigned long)data_size, total_chunks);

  if (s_art_bitmap) {
    gbitmap_destroy(s_art_bitmap);
    s_art_bitmap = NULL;
    s_art_data = NULL;
  }

  GBitmapFormat fmt = PBL_IF_COLOR_ELSE(GBitmapFormat8Bit, GBitmapFormat1Bit);
  s_art_bitmap = gbitmap_create_blank(GSize(width, height), fmt);
  if (!s_art_bitmap) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate bitmap %dx%d", width, height);
    if (s_callbacks.on_status) s_callbacks.on_status("Error: no memory");
    return;
  }

  s_art_data = gbitmap_get_data(s_art_bitmap);
  // Use actual bitmap capacity, not JS-reported size
  uint16_t row_bytes = gbitmap_get_bytes_per_row(s_art_bitmap);
  s_image_data_size = (uint32_t)row_bytes * height;
  s_total_chunks = total_chunks;
  s_received_chunks = 0;
  s_bytes_received = 0;

  snprintf(s_status_buf, sizeof(s_status_buf), "Receiving: 0/%d", total_chunks);
  if (s_callbacks.on_status) s_callbacks.on_status(s_status_buf);
}

static void handle_image_chunk(DictionaryIterator *iter) {
  Tuple *idx_tuple = dict_find(iter, MESSAGE_KEY_ImageChunkIndex);
  Tuple *data_tuple = dict_find(iter, MESSAGE_KEY_ImageChunkData);

  if (!idx_tuple || !data_tuple) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Incomplete chunk message");
    return;
  }

  if (!s_art_bitmap || !s_art_data) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Chunk received but no bitmap allocated");
    return;
  }

  uint16_t chunk_idx = idx_tuple->value->uint16;
  uint16_t data_len = data_tuple->length;

  APP_LOG(APP_LOG_LEVEL_INFO, "Chunk %d: %d bytes (received %lu/%lu)",
          chunk_idx, data_len,
          (unsigned long)(s_bytes_received + data_len),
          (unsigned long)s_image_data_size);

  if (s_bytes_received + data_len > s_image_data_size) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Chunk would overflow buffer!");
    return;
  }

  memcpy(s_art_data + s_bytes_received, data_tuple->value->data, data_len);
  s_bytes_received += data_len;
  s_received_chunks++;

  snprintf(s_status_buf, sizeof(s_status_buf), "Receiving: %d/%d",
           s_received_chunks, s_total_chunks);
  if (s_callbacks.on_status) s_callbacks.on_status(s_status_buf);

  if (s_received_chunks >= s_total_chunks) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Image complete! %lu bytes received",
            (unsigned long)s_bytes_received);
    if (s_callbacks.on_status) s_callbacks.on_status("Art loaded!");
    if (s_callbacks.on_image_ready) s_callbacks.on_image_ready(s_art_bitmap);
  }
}

static void handle_track_info(DictionaryIterator *iter) {
  Tuple *title_t = dict_find(iter, MESSAGE_KEY_TrackTitle);
  Tuple *artist_t = dict_find(iter, MESSAGE_KEY_TrackArtist);
  Tuple *dur_t = dict_find(iter, MESSAGE_KEY_TrackDuration);
  Tuple *elapsed_t = dict_find(iter, MESSAGE_KEY_TrackElapsed);
  Tuple *playing_t = dict_find(iter, MESSAGE_KEY_TrackIsPlaying);
  Tuple *shuffle_t = dict_find(iter, MESSAGE_KEY_ShuffleState);
  Tuple *repeat_t = dict_find(iter, MESSAGE_KEY_RepeatState);

  const char *title = title_t ? title_t->value->cstring : "";
  const char *artist = artist_t ? artist_t->value->cstring : "";
  int duration = dur_t ? (int)dur_t->value->int32 : 0;
  int elapsed = elapsed_t ? (int)elapsed_t->value->int32 : 0;
  bool is_playing = playing_t ? (playing_t->value->int32 != 0) : false;
  bool shuffle = shuffle_t ? (shuffle_t->value->int32 != 0) : false;
  int repeat_state = repeat_t ? (int)repeat_t->value->int32 : 0;

  APP_LOG(APP_LOG_LEVEL_INFO, "Track: %s - %s (%d/%d) %s shuf=%d rep=%d",
          title, artist, elapsed, duration, is_playing ? "playing" : "paused",
          shuffle ? 1 : 0, repeat_state);

  if (s_callbacks.on_track_info) {
    s_callbacks.on_track_info(title, artist, duration, elapsed, is_playing,
                              shuffle, repeat_state);
  }
}

static void handle_list_item(DictionaryIterator *iter) {
  Tuple *type_t = dict_find(iter, MESSAGE_KEY_ListType);
  Tuple *count_t = dict_find(iter, MESSAGE_KEY_ListCount);
  Tuple *index_t = dict_find(iter, MESSAGE_KEY_ListIndex);
  Tuple *title_t = dict_find(iter, MESSAGE_KEY_ListItemTitle);
  Tuple *sub_t = dict_find(iter, MESSAGE_KEY_ListItemSubtitle);
  Tuple *uri_t = dict_find(iter, MESSAGE_KEY_ListItemUri);

  int list_type = type_t ? (int)type_t->value->int32 : 0;
  int count = count_t ? (int)count_t->value->int32 : 0;
  int index = index_t ? (int)index_t->value->int32 : 0;
  const char *title = title_t ? title_t->value->cstring : "";
  const char *subtitle = sub_t ? sub_t->value->cstring : "";
  const char *uri = uri_t ? uri_t->value->cstring : "";

  APP_LOG(APP_LOG_LEVEL_INFO, "List item %d/%d: title='%s' sub='%s'",
          index + 1, count, title, subtitle);

  if (s_callbacks.on_list_item) {
    s_callbacks.on_list_item(list_type, index, count, title, subtitle, uri);
  }
}

static void handle_list_done(DictionaryIterator *iter) {
  Tuple *type_t = dict_find(iter, MESSAGE_KEY_ListType);
  int list_type = type_t ? (int)type_t->value->int32 : 0;

  APP_LOG(APP_LOG_LEVEL_INFO, "List done (type %d)", list_type);

  if (s_callbacks.on_list_done) {
    s_callbacks.on_list_done(list_type);
  }
}

static void handle_auth_status(DictionaryIterator *iter) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_AuthStatus);
  bool authed = t ? (t->value->int32 != 0) : false;

  APP_LOG(APP_LOG_LEVEL_INFO, "Auth status: %s", authed ? "authenticated" : "not authenticated");

  if (s_callbacks.on_auth_status) {
    s_callbacks.on_auth_status(authed);
  }
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  // JS ready signal
  if (dict_find(iter, MESSAGE_KEY_JsReady)) {
    handle_js_ready();
    return;
  }

  // Error message from JS
  if (dict_find(iter, MESSAGE_KEY_ErrorMsg)) {
    handle_error_msg(iter);
    return;
  }

  // Informational status message from JS
  if (dict_find(iter, MESSAGE_KEY_StatusMsg)) {
    handle_status_msg(iter);
    return;
  }

  // Track info from Spotify
  if (dict_find(iter, MESSAGE_KEY_TrackTitle)) {
    handle_track_info(iter);
    return;
  }

  // List item
  if (dict_find(iter, MESSAGE_KEY_ListIndex)) {
    handle_list_item(iter);
    return;
  }

  // List done
  if (dict_find(iter, MESSAGE_KEY_ListDone)) {
    handle_list_done(iter);
    return;
  }

  // Auth status
  if (dict_find(iter, MESSAGE_KEY_AuthStatus)) {
    handle_auth_status(iter);
    return;
  }

  // Image header
  if (dict_find(iter, MESSAGE_KEY_ImageWidth)) {
    handle_image_header(iter);
    return;
  }

  // Image chunk
  if (dict_find(iter, MESSAGE_KEY_ImageChunkIndex)) {
    handle_image_chunk(iter);
    return;
  }

  APP_LOG(APP_LOG_LEVEL_WARNING, "Unknown message received");
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", reason);
  snprintf(s_status_buf, sizeof(s_status_buf), "Msg dropped: %d", reason);
  if (s_callbacks.on_status) s_callbacks.on_status(s_status_buf);
}

static void outbox_sent_handler(DictionaryIterator *iter, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Outbox sent OK");
}

static void outbox_failed_handler(DictionaryIterator *iter,
                                   AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", reason);
  snprintf(s_status_buf, sizeof(s_status_buf), "Send failed: %d", reason);
  if (s_callbacks.on_status) s_callbacks.on_status(s_status_buf);
}

void comm_init(CommCallbacks callbacks) {
  s_callbacks = callbacks;

  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_sent(outbox_sent_handler);
  app_message_register_outbox_failed(outbox_failed_handler);

#if defined(PBL_PLATFORM_CHALK)
  // Chalk has ~49KB free heap and a 180x180x8bit album art bitmap
  // already costs ~32KB. The max AppMessage buffers (~8KB inbox + ~8KB
  // outbox) push us over the edge, so trim them: inbox just needs to
  // hold one 4096-byte image chunk + dict overhead, outbox only ever
  // carries tiny command messages.
  uint32_t inbox = 4200;
  uint32_t outbox = 256;
#else
  uint32_t inbox = app_message_inbox_size_maximum();
  uint32_t outbox = app_message_outbox_size_maximum();
#endif
  APP_LOG(APP_LOG_LEVEL_INFO, "AppMessage open: inbox=%lu outbox=%lu",
          (unsigned long)inbox, (unsigned long)outbox);

  app_message_open(inbox, outbox);
}

void comm_deinit(void) {
  app_message_deregister_callbacks();
  if (s_art_bitmap) {
    gbitmap_destroy(s_art_bitmap);
    s_art_bitmap = NULL;
  }
}

static void send_command_msg(AppCommand cmd, const char *context) {
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);

  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox begin failed: %d", result);
    return;
  }

  dict_write_int32(iter, MESSAGE_KEY_Command, (int32_t)cmd);
  if (context) {
    dict_write_cstring(iter, MESSAGE_KEY_CommandContext, context);
  }

  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed: %d", result);
  }
}

void comm_send_command(AppCommand cmd, const char *context) {
  if (!s_js_ready) {
    s_queued_cmd = cmd;
    if (context) {
      strncpy(s_queued_context, context, sizeof(s_queued_context) - 1);
      s_queued_context[sizeof(s_queued_context) - 1] = '\0';
    } else {
      s_queued_context[0] = '\0';
    }
    s_has_queued_command = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "JS not ready, queuing command %d", cmd);
    if (s_callbacks.on_status) s_callbacks.on_status("Waiting for phone...");
    return;
  }

  send_command_msg(cmd, context);
}

bool comm_is_js_ready(void) {
  return s_js_ready;
}

GBitmap *comm_get_cached_art(void) {
  return s_art_bitmap;
}
