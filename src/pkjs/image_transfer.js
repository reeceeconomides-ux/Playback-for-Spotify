var jpeg = require('jpeg-js');

function getChunkSize() {
  try {
    var platform = Pebble.getActiveWatchInfo().platform || '';
    if (platform === 'basalt' || platform === 'emery' || platform === 'gabbro') return 2000;
  } catch (e) {}
  return 1000;
}

var transferInProgress = false;
var lastUrl = null;

function isColorPlatform() {
  try {
    var platform = Pebble.getActiveWatchInfo().platform || '';
    return ['basalt', 'chalk', 'emery', 'gabbro'].indexOf(platform) !== -1;
  } catch (e) {
    return true;
  }
}

function sendError(msg) {
  console.log('[playback] ERROR: ' + msg);
  try {
    Pebble.sendAppMessage({ 'ErrorMsg': msg });
  } catch (e) {}
}

function arrayBufferToBytes(ab) {
  return new Uint8Array(ab);
}

function resizeAndQuantize(srcPixels, srcW, srcH, dstW, dstH) {
  var dst = new Uint8Array(dstW * dstH);
  var scale = Math.max(dstW / srcW, dstH / srcH);
  var scaledW = srcW * scale, scaledH = srcH * scale;
  var offsetX = (dstW - scaledW) / 2, offsetY = (dstH - scaledH) / 2;

  for (var y = 0; y < dstH; y++) {
    for (var x = 0; x < dstW; x++) {
      var srcX = Math.floor((x - offsetX) / scale);
      var srcY = Math.floor((y - offsetY) / scale);
      var r = 0, g = 0, b = 0;
      if (srcX >= 0 && srcX < srcW && srcY >= 0 && srcY < srcH) {
        var srcIdx = (srcY * srcW + srcX) * 4;
        r = srcPixels[srcIdx]; g = srcPixels[srcIdx + 1]; b = srcPixels[srcIdx + 2];
      }
      dst[y * dstW + x] = 0xC0 | ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6);
    }
  }
  return dst;
}

function resizeAndDither(srcPixels, srcW, srcH, dstW, dstH) {
  var gray = new Float32Array(dstW * dstH);
  var scale = Math.max(dstW / srcW, dstH / srcH);
  var scaledW = srcW * scale, scaledH = srcH * scale;
  var offsetX = (dstW - scaledW) / 2, offsetY = (dstH - scaledH) / 2;

  for (var y = 0; y < dstH; y++) {
    for (var x = 0; x < dstW; x++) {
      var srcX = Math.floor((x - offsetX) / scale);
      var srcY = Math.floor((y - offsetY) / scale);
      var lum = 0;
      if (srcX >= 0 && srcX < srcW && srcY >= 0 && srcY < srcH) {
        var srcIdx = (srcY * srcW + srcX) * 4;
        lum = 0.299 * srcPixels[srcIdx] + 0.587 * srcPixels[srcIdx+1] + 0.114 * srcPixels[srcIdx+2];
      }
      gray[y * dstW + x] = lum;
    }
  }

  for (var y = 0; y < dstH; y++) {
    for (var x = 0; x < dstW; x++) {
      var idx = y * dstW + x;
      var old = gray[idx];
      var nw = old < 128 ? 0 : 255;
      gray[idx] = nw;
      var err = old - nw;
      if (x + 1 < dstW) gray[idx + 1] += err * 7 / 16;
      if (y + 1 < dstH) {
        if (x > 0) gray[(y + 1) * dstW + (x - 1)] += err * 3 / 16;
        gray[(y + 1) * dstW + x] += err * 5 / 16;
        if (x + 1 < dstW) gray[(y + 1) * dstW + (x + 1)] += err * 1 / 16;
      }
    }
  }

  var rowBytes = Math.ceil(dstW / 32) * 4;
  var dst = new Uint8Array(rowBytes * dstH);
  for (var y = 0; y < dstH; y++) {
    for (var x = 0; x < dstW; x++) {
      if (gray[y * dstW + x] <= 128) {
        dst[y * rowBytes + Math.floor(x / 8)] |= (1 << (x % 8));
      }
    }
  }
  return dst;
}

function resizeAndQuantizeFit(srcPixels, srcW, srcH, dstW, dstH) {
  var dst = new Uint8Array(dstW * dstH);
  for (var i = 0; i < dst.length; i++) dst[i] = 0xC0;
  var scale = Math.min(dstW / srcW, dstH / srcH);
  var scaledW = srcW * scale, scaledH = srcH * scale;
  var offsetX = (dstW - scaledW) / 2, offsetY = (dstH - scaledH) / 2;

  for (var y = 0; y < dstH; y++) {
    for (var x = 0; x < dstW; x++) {
      var srcX = Math.floor((x - offsetX) / scale);
      var srcY = Math.floor((y - offsetY) / scale);
      if (srcX >= 0 && srcX < srcW && srcY >= 0 && srcY < srcH) {
        var srcIdx = (srcY * srcW + srcX) * 4;
        var r = srcPixels[srcIdx], g = srcPixels[srcIdx+1], b = srcPixels[srcIdx+2];
        dst[y * dstW + x] = 0xC0 | ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6);
      }
    }
  }
  return dst;
}

function processJpeg(jpegData) {
  try {
    var raw = jpeg.decode(jpegData, { useTArray: true });
    var W = 144, H = 106, isRound = false;
    try {
      var platform = Pebble.getActiveWatchInfo().platform || '';
      if (platform === 'emery') { W = 200; H = 166; }
      else if (platform === 'gabbro') { W = 260; H = 260; isRound = true; }
      else if (platform === 'chalk') { W = 180; H = 180; isRound = true; }
    } catch (e) {}

    var imageData = isRound ? resizeAndQuantizeFit(raw.data, raw.width, raw.height, W, H)
                 : isColorPlatform() ? resizeAndQuantize(raw.data, raw.width, raw.height, W, H)
                 : resizeAndDither(raw.data, raw.width, raw.height, W, H);

    sendImageToWatch(imageData, W, H);
  } catch (ex) {
    lastUrl = null;
    sendError('Decode: ' + ex.message);
  }
}

function downloadAndSend(url) {
  if (transferInProgress || url === lastUrl) return;
  lastUrl = url;
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.responseType = 'arraybuffer';
  xhr.onload = function() {
    if (xhr.status === 200) {
      processJpeg(arrayBufferToBytes(xhr.response));
    } else {
      // Don't pin lastUrl to a URL we failed to fetch — otherwise the
      // early-return guard blocks every future retry for that URL.
      lastUrl = null;
      sendError('Download ' + xhr.status);
    }
  };
  xhr.onerror = function() {
    lastUrl = null;
    sendError('Download failed');
  };
  xhr.send();
}

function sendImageToWatch(data, width, height) {
  transferInProgress = true;
  var chunkSize = getChunkSize();
  var totalChunks = Math.ceil(data.length / chunkSize);

  Pebble.sendAppMessage({
    'ImageWidth': width,
    'ImageHeight': height,
    'ImageDataSize': data.length,
    'ImageChunksTotal': totalChunks
  }, function() {
    sendChunk(data, 0, totalChunks, chunkSize);
  }, function() {
    transferInProgress = false;
    lastUrl = null;
    sendError('Header fail');
  });
}

function sendChunk(data, index, totalChunks, chunkSize) {
  var start = index * chunkSize;
  var end = Math.min(start + chunkSize, data.length);
  
  // Convert Uint8Array to standard Array for emulator compatibility
  var chunk = [];
  for (var i = start; i < end; i++) {
    chunk.push(data[i]);
  }

  Pebble.sendAppMessage({
    'ImageChunkIndex': index,
    'ImageChunkData': chunk
  }, function() {
    if (index + 1 < totalChunks) sendChunk(data, index + 1, totalChunks, chunkSize);
    else transferInProgress = false;
  }, function() {
    // Retry once
    setTimeout(function() {
      Pebble.sendAppMessage({
        'ImageChunkIndex': index,
        'ImageChunkData': chunk
      }, function() {
        if (index + 1 < totalChunks) sendChunk(data, index + 1, totalChunks, chunkSize);
        else transferInProgress = false;
      }, function() {
        transferInProgress = false;
        lastUrl = null;
        sendError('Chunk fail');
      });
    }, 500);
  });
}

module.exports = {
  sendImageFromUrl: downloadAndSend,
  isTransferring: function() { return transferInProgress; }
};
