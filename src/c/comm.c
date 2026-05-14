#include "comm.h"

static CommCallbacks s_callbacks;

static GBitmap *s_art_bitmap = NULL;       // displayed (complete) art
static GBitmap *s_art_bitmap_prev = NULL;  // previous art kept alive during transition
static GBitmap *s_art_pending = NULL;      // in-progress incoming art
static uint8_t *s_art_data = NULL;
static uint32_t s_image_data_size = 0;
static uint16_t s_total_chunks = 0;
static uint16_t s_received_chunks = 0;
static uint32_t s_bytes_received = 0;

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
// Slot-1 (next-track prefetch) state. Filled in the background while the
// current track plays; promoted to s_art_bitmap when the user advances.
static GBitmap *s_art_next_pending = NULL;
static GBitmap *s_art_next_ready = NULL;
static uint8_t *s_art_next_data = NULL;
static uint32_t s_next_image_data_size = 0;
static uint16_t s_next_total_chunks = 0;
static uint16_t s_next_received_chunks = 0;
static uint32_t s_next_bytes_received = 0;
#endif

// JS ready handshake
static bool s_js_ready = false;
static bool s_has_queued_command = false;
static AppCommand s_queued_cmd;
static char s_queued_context[128];

static char s_status_buf[64];

static void send_command_msg(AppCommand cmd, const char *context);

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
// Tell JS to drop its prefetch state. Used when the watch can't allocate a
// slot-1 bitmap (tight heap) or when a promote arrives with nothing ready —
// JS otherwise sets prefetchReady=true on chunk ACKs (the OS ACKs even when
// the app dropped the dict), causing fast-path promotes to no-op forever
// and leaving the screen stuck on the previously-displayed track.
static void send_prefetch_abort(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint8(out, MESSAGE_KEY_PrefetchAbort, 1);
    app_message_outbox_send();
  }
}
#endif

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
  Tuple *slot_tuple = dict_find(iter, MESSAGE_KEY_ImageSlot);
  uint8_t slot = slot_tuple ? slot_tuple->value->uint8 : 0;

  if (!w_tuple || !h_tuple || !size_tuple || !chunks_tuple) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Incomplete image header");
    return;
  }

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  if (slot == 1) {
    uint16_t width = w_tuple->value->uint16;
    uint16_t height = h_tuple->value->uint16;
    uint16_t total_chunks = chunks_tuple->value->uint16;
    APP_LOG(APP_LOG_LEVEL_INFO, "Prefetch header: %dx%d, %d chunks", width, height, total_chunks);

    if (s_art_next_pending) {
      gbitmap_destroy(s_art_next_pending);
      s_art_next_pending = NULL;
      s_art_next_data = NULL;
    }
    if (s_art_next_ready) {
      gbitmap_destroy(s_art_next_ready);
      s_art_next_ready = NULL;
    }

    // Heap budget: gabbro at SDK 4.9.169 has a 128 KB slot, not the 1 MB of
    // firmware 4.9.171. A 260x260x8bit bitmap is 67 KB; two of them won't
    // fit alongside code+AppMessage. Skip prefetch when heap is tight and
    // tell JS so it doesn't mark prefetchReady=true on the chunk ACKs.
    size_t need = (size_t)width * height + 256;
    size_t avail = heap_bytes_free();
    if (avail < need + 8192) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Skip prefetch: heap=%u need=%u+8k",
              (unsigned)avail, (unsigned)need);
      send_prefetch_abort();
      return;
    }

    s_art_next_pending = gbitmap_create_blank(GSize(width, height), GBitmapFormat8Bit);
    if (!s_art_next_pending) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate prefetch bitmap %dx%d", width, height);
      send_prefetch_abort();
      return;
    }
    s_art_next_data = gbitmap_get_data(s_art_next_pending);
    uint16_t row_bytes = gbitmap_get_bytes_per_row(s_art_next_pending);
    s_next_image_data_size = (uint32_t)row_bytes * height;
    s_next_total_chunks = total_chunks;
    s_next_received_chunks = 0;
    s_next_bytes_received = 0;
    return;
  }
  // Slot-0 header arrival proves the new track did not match the
  // prefetched URI (fast path would have sent ImagePromote instead).
  // The prefetched bitmap is for a forward direction the user is not
  // taking — free it so s_art_pending has heap to allocate into.
  comm_drop_prefetch();

  // If heap is still too tight to hold current + pending simultaneously
  // (gabbro on 128 KB slot: two 67 KB bitmaps don't fit), destroy the
  // displayed bitmap before allocating. Gives up the visual continuity
  // Phase 1 was meant to add, but only when the heap actually demands it.
  {
    size_t need = (size_t)w_tuple->value->uint16 * h_tuple->value->uint16 + 256;
    if (s_art_bitmap && heap_bytes_free() < need + 4096) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Tight heap: dropping current art before alloc (heap=%u need=%u)",
              (unsigned)heap_bytes_free(), (unsigned)need);
      if (s_callbacks.on_image_ready) s_callbacks.on_image_ready(NULL);
      gbitmap_destroy(s_art_bitmap);
      s_art_bitmap = NULL;
      if (s_art_bitmap_prev) {
        gbitmap_destroy(s_art_bitmap_prev);
        s_art_bitmap_prev = NULL;
      }
    }
  }
#else
  (void)slot;
#endif

#if !defined(PBL_PLATFORM_EMERY) && !defined(PBL_PLATFORM_GABBRO) && !defined(PBL_PLATFORM_APLITE)
  // On memory-constrained platforms, we cannot hold two images in memory.
  // Instantly clear the screen and destroy the old image before allocating the new one.
  // EXCLUDED on aplite because the UI may still hold a reference to s_art_bitmap
  // via an in-flight animation (s_art_pending_bmp); freeing here causes a
  // use-after-free when the animation midpoint fires apply_art_content().
  // Aplite has enough free heap (≥4 KB) to keep 2× 1152 B bitmaps simultaneously.
  // EXCLUDED on gabbro since the 1 MB app slot in PebbleOS v4.9.171 leaves plenty
  // of room for two color bitmaps (current + pending). The prefetch path also
  // requires keeping the showing bitmap alive while slot-1 fills.
  if (s_art_bitmap) {
    if (s_callbacks.on_image_ready) s_callbacks.on_image_ready(NULL);
    gbitmap_destroy(s_art_bitmap);
    s_art_bitmap = NULL;
  }
  if (s_art_bitmap_prev) {
    gbitmap_destroy(s_art_bitmap_prev);
    s_art_bitmap_prev = NULL;
  }
#endif

  uint16_t width = w_tuple->value->uint16;
  uint16_t height = h_tuple->value->uint16;
  uint32_t data_size = size_tuple->value->uint32;
  uint16_t total_chunks = chunks_tuple->value->uint16;

  APP_LOG(APP_LOG_LEVEL_INFO, "Image header: %dx%d, %lu bytes, %d chunks",
          width, height, (unsigned long)data_size, total_chunks);

  if (s_art_pending) {
    gbitmap_destroy(s_art_pending);
    s_art_pending = NULL;
    s_art_data = NULL;
  }

  GBitmapFormat fmt = PBL_IF_COLOR_ELSE(GBitmapFormat8Bit, GBitmapFormat1Bit);
  s_art_pending = gbitmap_create_blank(GSize(width, height), fmt);
  if (!s_art_pending) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate bitmap %dx%d", width, height);
    if (s_callbacks.on_status) s_callbacks.on_status("Error: no memory");
    return;
  }

  s_art_data = gbitmap_get_data(s_art_pending);
  // Use actual bitmap capacity, not JS-reported size
  uint16_t row_bytes = gbitmap_get_bytes_per_row(s_art_pending);
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
  Tuple *slot_tuple = dict_find(iter, MESSAGE_KEY_ImageSlot);
  uint8_t slot = slot_tuple ? slot_tuple->value->uint8 : 0;

  if (!idx_tuple || !data_tuple) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Incomplete chunk message");
    return;
  }

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  if (slot == 1) {
    if (!s_art_next_pending || !s_art_next_data) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Prefetch chunk with no bitmap allocated");
      return;
    }
    uint16_t data_len = data_tuple->length;
    if (s_next_bytes_received + data_len > s_next_image_data_size) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Prefetch chunk would overflow buffer");
      return;
    }
    memcpy(s_art_next_data + s_next_bytes_received, data_tuple->value->data, data_len);
    s_next_bytes_received += data_len;
    s_next_received_chunks++;
    if (s_next_received_chunks >= s_next_total_chunks) {
      APP_LOG(APP_LOG_LEVEL_INFO, "Prefetch ready: %lu bytes", (unsigned long)s_next_bytes_received);
      s_art_next_ready = s_art_next_pending;
      s_art_next_pending = NULL;
      s_art_next_data = NULL;
    }
    return;
  }
#else
  (void)slot;
#endif

  if (!s_art_pending || !s_art_data) {
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
    // Keep old bitmap alive until ui.c finishes the exit-phase animation.
    // comm_release_old_art() is called by ui.c at the animation midpoint.
    if (s_art_bitmap_prev) {
      gbitmap_destroy(s_art_bitmap_prev); // previous transition completed or was skipped
    }
    s_art_bitmap_prev = s_art_bitmap;     // hand off; do NOT destroy yet
    s_art_bitmap = s_art_pending;
    s_art_pending = NULL;
    s_art_data = NULL;
    if (s_callbacks.on_image_ready) s_callbacks.on_image_ready(s_art_bitmap);
  }
}

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
static void handle_image_promote(void) {
  if (!s_art_next_ready) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Promote: no prefetched art ready");
    // JS thought slot 1 was filled but it isn't (probably an earlier
    // heap-skip). Tell JS to drop its state so the next poll falls into
    // the slot-0 slow path instead of skipping on lastImageUrl equality.
    send_prefetch_abort();
    return;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "Promoting prefetched art");
  if (s_art_bitmap_prev) {
    gbitmap_destroy(s_art_bitmap_prev);
  }
  s_art_bitmap_prev = s_art_bitmap;
  s_art_bitmap = s_art_next_ready;
  s_art_next_ready = NULL;
  if (s_callbacks.on_image_ready) s_callbacks.on_image_ready(s_art_bitmap);
}
#endif

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

  // Promote prefetched art to current
  if (dict_find(iter, MESSAGE_KEY_ImagePromote)) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
    handle_image_promote();
#endif
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

#if defined(PBL_PLATFORM_APLITE)
  // Aplite has only 24 KB total app slot. Image chunks are 1000 B,
  // so a tight inbox is enough; outbox only carries small command messages.
  uint32_t inbox = 1200;
  uint32_t outbox = 256;
#elif defined(PBL_PLATFORM_CHALK)
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

void comm_release_old_art(void) {
  if (s_art_bitmap_prev) {
    gbitmap_destroy(s_art_bitmap_prev);
    s_art_bitmap_prev = NULL;
  }
}

void comm_free_all_art(void) {
  if (s_art_pending) {
    gbitmap_destroy(s_art_pending);
    s_art_pending = NULL;
    s_art_data = NULL;
  }
  if (s_art_bitmap_prev) {
    gbitmap_destroy(s_art_bitmap_prev);
    s_art_bitmap_prev = NULL;
  }
  if (s_art_bitmap) {
    gbitmap_destroy(s_art_bitmap);
    s_art_bitmap = NULL;
  }
  comm_drop_prefetch();
}

void comm_drop_prefetch(void) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  if (s_art_next_pending) {
    gbitmap_destroy(s_art_next_pending);
    s_art_next_pending = NULL;
    s_art_next_data = NULL;
  }
  if (s_art_next_ready) {
    gbitmap_destroy(s_art_next_ready);
    s_art_next_ready = NULL;
  }
#endif
}

void comm_deinit(void) {
  app_message_deregister_callbacks();
  if (s_art_pending) {
    gbitmap_destroy(s_art_pending);
    s_art_pending = NULL;
  }
  if (s_art_bitmap_prev) {
    gbitmap_destroy(s_art_bitmap_prev);
    s_art_bitmap_prev = NULL;
  }
  if (s_art_bitmap) {
    gbitmap_destroy(s_art_bitmap);
    s_art_bitmap = NULL;
  }
  comm_drop_prefetch();
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
