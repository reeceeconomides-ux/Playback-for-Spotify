// Spotify OAuth - Implicit Grant via Pebble config page

var SPOTIFY_CLIENT_ID = '';
var SPOTIFY_REDIRECT_URI = '';
var SPOTIFY_SCOPES = 'user-read-playback-state user-modify-playback-state user-read-currently-playing playlist-read-private user-library-read user-library-modify user-follow-read';

var TOKEN_KEY = 'spotify_token';
var EXPIRY_KEY = 'spotify_token_expiry';
var REFRESH_KEY = 'spotify_refresh_token';
var CLIENT_ID_KEY = 'spotify_client_id';

var refreshInProgress = false;
var refreshQueue = [];

function saveToken(token, expiresIn, clientId, refreshToken) {
  var expiryTime = Date.now() + (expiresIn * 1000);
  localStorage.setItem(TOKEN_KEY, token);
  localStorage.setItem(EXPIRY_KEY, String(expiryTime));
  if (clientId) {
    localStorage.setItem(CLIENT_ID_KEY, clientId);
  }
  if (refreshToken) {
    localStorage.setItem(REFRESH_KEY, refreshToken);
  }
}

function getToken() {
  var token = localStorage.getItem(TOKEN_KEY);
  var expiry = parseInt(localStorage.getItem(EXPIRY_KEY), 10);
  // Return token even if expired, apiRequest will trigger refresh
  return token;
}

function isTokenExpired() {
  var expiry = parseInt(localStorage.getItem(EXPIRY_KEY), 10);
  return !expiry || Date.now() > (expiry - 60000); // 1 min buffer
}

function refreshAccessToken(callback) {
  // If a refresh is already in progress, queue this callback
  if (refreshInProgress) {
    refreshQueue.push(callback);
    return;
  }

  var refreshToken = localStorage.getItem(REFRESH_KEY);
  var clientId = localStorage.getItem(CLIENT_ID_KEY) || SPOTIFY_CLIENT_ID;

  if (!refreshToken) {
    console.log('[playback] No refresh token available');
    if (callback) callback(new Error('No refresh token'), null);
    return;
  }

  if (!clientId) {
    console.log('[playback] No client ID available');
    if (callback) callback(new Error('No client ID'), null);
    return;
  }

  refreshInProgress = true;
  console.log('[playback] Refreshing access token...');
  var xhr = new XMLHttpRequest();
  xhr.open('POST', 'https://accounts.spotify.com/api/token', true);
  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');

  var body = 'grant_type=refresh_token' +
             '&refresh_token=' + encodeURIComponent(refreshToken) +
             '&client_id=' + encodeURIComponent(clientId);

  xhr.onload = function() {
    refreshInProgress = false;
    var queued = refreshQueue.slice();
    refreshQueue = [];

    if (xhr.status === 200) {
      var data = JSON.parse(xhr.responseText);
      saveToken(data.access_token, data.expires_in, clientId, data.refresh_token);
      console.log('[playback] Token refreshed successfully');
      if (callback) callback(null, data.access_token);
      for (var i = 0; i < queued.length; i++) {
        if (queued[i]) queued[i](null, data.access_token);
      }
    } else {
      console.log('[playback] Refresh failed: ' + xhr.status + ' ' + xhr.responseText);
      var err = new Error('Refresh failed');
      if (callback) callback(err, null);
      for (var i = 0; i < queued.length; i++) {
        if (queued[i]) queued[i](err, null);
      }
    }
  };
  xhr.onerror = function() {
    refreshInProgress = false;
    var queued = refreshQueue.slice();
    refreshQueue = [];
    var err = new Error('Network error');
    if (callback) callback(err, null);
    for (var i = 0; i < queued.length; i++) {
      if (queued[i]) queued[i](err, null);
    }
  };
  xhr.send(body);
}

function clearToken() {
  localStorage.removeItem(TOKEN_KEY);
  localStorage.removeItem(EXPIRY_KEY);
  localStorage.removeItem(REFRESH_KEY);
}

function isAuthenticated() {
  return localStorage.getItem(REFRESH_KEY) !== null;
}

function init(onAuthChange) {
  Pebble.addEventListener('showConfiguration', function() {
    var savedClientId = localStorage.getItem(CLIENT_ID_KEY) || '';
    var configUrl = SPOTIFY_REDIRECT_URI + '?t=' + Date.now();
    if (savedClientId) {
      configUrl += '&client_id=' + encodeURIComponent(savedClientId);
    }
    console.log('[playback] Opening config page: ' + configUrl);
    Pebble.openURL(configUrl);
  });

  Pebble.addEventListener('webviewclosed', function(e) {
    if (e && e.response) {
      try {
        var decoded = decodeURIComponent(e.response);
        var data = JSON.parse(decoded);
        if (data.token) {
          var expiresIn = data.expiresIn || 3600;
          saveToken(data.token, expiresIn, data.clientId, data.refreshToken);
          console.log('[playback] Token and Refresh Token saved');
          if (onAuthChange) onAuthChange(true);
        }
      } catch (ex) {
        console.log('[playback] Config parse error: ' + ex.message);
      }
    }
  });
}

module.exports = {
  init: init,
  getToken: getToken,
  isTokenExpired: isTokenExpired,
  refreshAccessToken: refreshAccessToken,
  clearToken: clearToken,
  isAuthenticated: isAuthenticated
};
