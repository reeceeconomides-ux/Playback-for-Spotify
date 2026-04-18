var auth = require('./spotify_auth');
var api = require('./spotify_api');
var imageTransfer = require('./image_transfer');

var pollTimer = null;
var POLL_INTERVAL = 5000;
var currentVolume = 50;
var lastImageUrl = null;
var listSendInProgress = false;
var lastFetchedItems = [];
var lastFetchedListType = -1;
var lastShuffleState = false;
var lastRepeatState = 0; // 0=off, 1=context, 2=track

// Target album-art size per platform
function getTargetArtSize() {
  try {
    var platform = (Pebble.getActiveWatchInfo().platform || '');
    if (platform === 'gabbro') return 260;
    if (platform === 'chalk')  return 180;
    if (platform === 'emery')  return 200;
    if (platform === 'basalt') return 168;
  } catch (e) {}
  return 168;
}

function pickImageUrl(images, target) {
  if (!images || images.length === 0) return null;
  var best = null;
  for (var i = 0; i < images.length; i++) {
    var img = images[i];
    var side = Math.max(img.width || 0, img.height || 0);
    if (side >= target) {
      if (!best || side < Math.max(best.width || 0, best.height || 0)) {
        best = img;
      }
    }
  }
  return (best || images[0]).url;
}

function noop() {}

function sendError(msg) {
  console.log('[playback] ERROR: ' + msg);
  try {
    Pebble.sendAppMessage({ 'ErrorMsg': msg }, null, null);
  } catch (e) { /* best effort */ }
}

function sendAuthStatus() {
  var authed = auth.isAuthenticated() ? 1 : 0;
  Pebble.sendAppMessage({ 'AuthStatus': authed }, null, null);
}

// --- Now Playing ---

function fetchNowPlaying() {
  if (listSendInProgress) return;
  api.getCurrentPlayback(function(err, data) {
    if (err) {
      console.log('[playback] Now playing error: ' + err.message);
      return;
    }
    if (!data || !data.item) return;

    var track = data.item;
    var isEpisode = (data.currently_playing_type === 'episode') || (track.type === 'episode');

    var artistLine = '';
    var artImages = null;
    if (isEpisode) {
      if (track.show) {
        artistLine = track.show.name || track.show.publisher || '';
      }
      artImages = (track.images && track.images.length) ? track.images
                : (track.show && track.show.images) ? track.show.images
                : null;
    } else {
      if (track.artists) {
        var names = [];
        for (var i = 0; i < track.artists.length; i++) {
          names.push(track.artists[i].name);
        }
        artistLine = names.join(', ');
      }
      if (track.album && track.album.images && track.album.images.length > 0) {
        artImages = track.album.images;
      }
    }

    if (artImages && artImages.length > 0) {
      var imageUrl = pickImageUrl(artImages, getTargetArtSize());
      if (imageUrl && imageUrl !== lastImageUrl && !imageTransfer.isTransferring()) {
        lastImageUrl = imageUrl;
        imageTransfer.sendImageFromUrl(imageUrl);
      }
    }

    lastShuffleState = !!data.shuffle_state;
    lastRepeatState = (data.repeat_state === 'track') ? 2
                    : (data.repeat_state === 'context') ? 1 : 0;

    Pebble.sendAppMessage({
      'TrackTitle': track.name || '',
      'TrackArtist': artistLine,
      'TrackDuration': Math.floor(track.duration_ms / 1000),
      'TrackElapsed': Math.floor((data.progress_ms || 0) / 1000),
      'TrackIsPlaying': data.is_playing ? 1 : 0,
      'ShuffleState': lastShuffleState ? 1 : 0,
      'RepeatState': lastRepeatState
    }, null, null);

    if (data.device) {
      currentVolume = data.device.volume_percent;
    }
  });
}

function startPolling() {
  if (pollTimer) return;
  fetchNowPlaying();
  pollTimer = setInterval(fetchNowPlaying, POLL_INTERVAL);
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

// --- List sending ---

function sendListItem(listType, items, index, count) {
  if (index >= count) {
    listSendInProgress = false;
    Pebble.sendAppMessage({
      'ListType': listType,
      'ListDone': 1
    }, null, null);
    return;
  }

  var item = items[index];
  Pebble.sendAppMessage({
    'ListType': listType,
    'ListCount': count,
    'ListIndex': index,
    'ListItemTitle': item.title || '',
    'ListItemSubtitle': item.subtitle || '',
    'ListItemUri': item.uri || ''
  }, function() {
    sendListItem(listType, items, index + 1, count);
  }, function() {
    setTimeout(function() {
      sendListItem(listType, items, index, count);
    }, 500);
  });
}

function fetchAndSendList(listType, apiFn, formatFn) {
  lastFetchedListType = listType;
  apiFn(function(err, data) {
    if (err) {
      sendError(err.message);
      if (err.message === 'Token expired') sendAuthStatus();
      return;
    }
    if (!data) {
      sendError('No data');
      return;
    }
    var items = formatFn(data);
    lastFetchedItems = items;
    var count = Math.min(items.length, 20);
    if (count === 0) {
      Pebble.sendAppMessage({
        'ListType': listType,
        'ListDone': 1
      }, null, null);
      return;
    }
    listSendInProgress = true;
    sendListItem(listType, items, 0, count);
  });
}

function formatPlaylists(data) {
  return (data.items || []).map(function(i) {
    return { title: i.name, subtitle: i.owner ? i.owner.display_name : '', uri: i.uri };
  });
}

function formatTracks(data) {
  return (data.items || []).map(function(i) {
    var t = i.track || i;
    return { title: t.name, subtitle: (t.artists && t.artists[0]) ? t.artists[0].name : '', uri: t.uri };
  });
}

function formatArtists(data) {
  return (data.artists && data.artists.items || []).map(function(i) {
    return { title: i.name, subtitle: (i.genres && i.genres[0]) || '', uri: i.uri };
  });
}

function formatShows(data) {
  return (data.items || []).map(function(i) {
    var s = i.show || i;
    return { title: s.name, subtitle: s.publisher || '', uri: s.uri };
  });
}

function formatAlbums(data) {
  return (data.items || []).map(function(i) {
    var a = i.album || i;
    return { title: a.name, subtitle: (a.artists && a.artists[0]) ? a.artists[0].name : '', uri: a.uri };
  });
}

function formatQueue(data) {
  // The API puts the now-playing track in `currently_playing` and the
  // *upcoming* entries in `queue`. Only upcoming is shown per the
  // product decision.
  return (data.queue || []).map(function(t) {
    if (!t) return { title: '', subtitle: '', uri: '' };
    var sub = '';
    if (t.type === 'episode' && t.show) {
      sub = t.show.name || '';
    } else if (t.artists && t.artists[0]) {
      sub = t.artists[0].name;
    }
    return { title: t.name || '', subtitle: sub, uri: t.uri || '' };
  });
}

// Resolve a Spotify URI to a single track/episode URI playable via the
// queue endpoint. The queue API only accepts track: or episode: URIs,
// so container URIs must be resolved to their first playable item.
function resolveToTrackUri(uri, cb) {
  if (!uri) return cb(new Error('empty uri'));
  if (uri.indexOf(':track:') !== -1 || uri.indexOf(':episode:') !== -1) {
    return cb(null, uri);
  }
  var parts = uri.split(':');
  var kind = parts[1], id = parts[2];
  if (kind === 'playlist') {
    api.getPlaylistFirstTrack(id, function(err, data) {
      if (err) return cb(err);
      var t = data && data.items && data.items[0] && data.items[0].track;
      cb(null, t ? t.uri : null);
    });
  } else if (kind === 'album') {
    api.getAlbumFirstTrack(id, function(err, data) {
      if (err) return cb(err);
      var t = data && data.items && data.items[0];
      cb(null, t ? t.uri : null);
    });
  } else if (kind === 'artist') {
    api.getArtistTopTrack(id, function(err, data) {
      if (err) return cb(err);
      var t = data && data.tracks && data.tracks[0];
      cb(null, t ? t.uri : null);
    });
  } else if (kind === 'show') {
    api.getShowEpisodes(id, function(err, data) {
      if (err) return cb(err);
      var e = data && data.items && data.items[0];
      cb(null, e ? e.uri : null);
    });
  } else {
    cb(new Error('unsupported uri'));
  }
}

// --- Command dispatch ---

function handleCommand(cmd, context) {
  switch (cmd) {
    case 1: // CMD_FETCH_NOW_PLAYING
      fetchNowPlaying();
      break;
    case 2: // CMD_FETCH_PLAYLISTS
      fetchAndSendList(0, api.getPlaylists, formatPlaylists);
      break;
    case 3: // CMD_FETCH_ARTISTS
      fetchAndSendList(1, api.getFollowedArtists, formatArtists);
      break;
    case 4: // CMD_FETCH_ALBUMS
      fetchAndSendList(2, api.getSavedAlbums, formatAlbums);
      break;
    case 5: // CMD_FETCH_LIKED_SONGS
      fetchAndSendList(3, api.getLikedTracks, formatTracks);
      break;
    case 6: // CMD_FETCH_SHOWS
      fetchAndSendList(4, api.getSavedShows, formatShows);
      break;
    case 8: // CMD_FETCH_QUEUE
      fetchAndSendList(5, api.getQueue, formatQueue);
      break;
    case 10: // CMD_PLAY_PAUSE
      api.getCurrentPlayback(function(err, data) {
        if (err) { sendError(err.message); return; }
        startPolling();
        if (data && data.is_playing) {
          api.pause(function() { setTimeout(fetchNowPlaying, 300); });
        } else {
          api.play(function() { setTimeout(fetchNowPlaying, 300); });
        }
      });
      break;
    case 11: // CMD_NEXT_TRACK
      api.nextTrack(function() { startPolling(); setTimeout(fetchNowPlaying, 500); });
      break;
    case 12: // CMD_PREV_TRACK
      api.previousTrack(function() { startPolling(); setTimeout(fetchNowPlaying, 500); });
      break;
    case 13: // CMD_VOLUME_UP
      currentVolume = Math.min(100, currentVolume + 10);
      api.setVolume(currentVolume, noop);
      break;
    case 14: // CMD_VOLUME_DOWN
      currentVolume = Math.max(0, currentVolume - 10);
      api.setVolume(currentVolume, noop);
      break;
    case 17: // CMD_SEEK_FORWARD
    case 18: // CMD_SEEK_BACK
      (function(delta) {
        api.getCurrentPlayback(function(err, data) {
          if (err || !data || !data.item) return;
          var pos = (data.progress_ms || 0) + delta;
          api.seek(Math.max(0, Math.min(data.item.duration_ms, pos)), function() { 
            setTimeout(fetchNowPlaying, 300); 
          });
        });
      })(cmd === 17 ? 15000 : -15000);
      break;
    case 20: // CMD_PLAY_CONTEXT
      if (context) {
        var uris = null;
        if (context.indexOf(':track:') !== -1) {
          uris = [];
          var found = false;
          for (var i = 0; i < lastFetchedItems.length; i++) {
            if (lastFetchedItems[i].uri === context) found = true;
            if (found) uris.push(lastFetchedItems[i].uri);
          }
          if (uris.length === 0) uris = [context];
        }
        api.playContext(uris ? null : context, function(err) {
          if (err) sendError(err.message);
          else setTimeout(fetchNowPlaying, 500);
        }, uris ? { uris: uris } : null);
      }
      break;
    case 24: // CMD_TOGGLE_SHUFFLE
      (function() {
        var next = !lastShuffleState;
        api.setShuffle(next, function(err) {
          if (err) { sendError('Shuffle: ' + err.message); return; }
          lastShuffleState = next;
          setTimeout(fetchNowPlaying, 300);
          if (lastFetchedListType === 5) {
            setTimeout(function() { fetchAndSendList(5, api.getQueue, formatQueue); }, 500);
          }
        });
      })();
      break;
    case 25: // CMD_CYCLE_REPEAT
      (function() {
        var next = (lastRepeatState + 1) % 3;
        var name = ['off', 'context', 'track'][next];
        api.setRepeat(name, function(err) {
          if (err) { sendError('Repeat: ' + err.message); return; }
          lastRepeatState = next;
          setTimeout(fetchNowPlaying, 300);
          if (lastFetchedListType === 5) {
            setTimeout(function() { fetchAndSendList(5, api.getQueue, formatQueue); }, 500);
          }
        });
      })();
      break;
    case 26: // CMD_QUEUE_ADD (context = URI — resolves non-tracks to first track)
      if (context) resolveToTrackUri(context, function(err, uri) {
        if (err || !uri) { sendError('Queue: ' + (err ? err.message : 'no track')); return; }
        api.addToQueue(uri, function(err2) {
          if (err2) sendError('Queue: ' + err2.message);
          else {
            Pebble.sendAppMessage({ 'StatusMsg': 'Added to queue' }, null, null);
            if (lastFetchedListType === 5) {
              setTimeout(function() { fetchAndSendList(5, api.getQueue, formatQueue); }, 500);
            }
          }
        });
      });
      break;
    case 21: // CMD_PLAY_SHOW
      if (context) {
        var marker = ':show:';
        var showId = context.substring(context.indexOf(marker) + marker.length);
        api.getShowEpisodes(showId, function(err, data) {
          if (err || !data || !data.items || !data.items[0]) { sendError('No episodes'); return; }
          api.playContext(null, function(err2) {
            if (!err2) { startPolling(); setTimeout(fetchNowPlaying, 500); }
          }, { uris: [data.items[0].uri] });
        });
      }
      break;
    case 30: // CMD_FETCH_ART
      if (context) imageTransfer.sendImageFromUrl(context);
      break;
  }
}

auth.init(function(authed) {
  sendAuthStatus();
  if (authed) startPolling();
  else stopPolling();
});

Pebble.addEventListener('ready', function() {
  Pebble.sendAppMessage({ 'JsReady': 1 });
  sendAuthStatus();
  if (auth.isAuthenticated()) {
    startPolling();
    setTimeout(fetchNowPlaying, 1000);
  }
});

Pebble.addEventListener('appmessage', function(e) {
  var cmd = e.payload['Command'] || e.payload[100];
  var ctx = e.payload['CommandContext'] || e.payload[101];
  if (cmd !== undefined && cmd !== null) handleCommand(cmd, ctx);
});
