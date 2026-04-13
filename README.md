# Playback for Spotify

Control Spotify from your Pebble smartwatch. Browse playlists, artists, albums and liked songs, skip tracks, adjust volume, and see album art — all from your wrist.
[Repebble AppStore link](https://apps.repebble.com/cd7c4d5fe7ec4989a64976c6)

![basalt menu](screenshots/basalt_1_menu.png) ![basalt now playing](screenshots/basalt_2_adele.png) ![basalt now playing 2](screenshots/basalt_3_weeknd.png)

## Features

- **Now Playing** — track title, artist, album art, and progress bar
- **Playback controls** — play/pause, next/previous track
- **Volume control** — hold UP/DOWN to adjust volume (currently only while playing on external speakers)
- **Browse library** — playlists, artists, albums, liked songs
- **Album art** — full-color on color watches, Floyd-Steinberg dithered on B&W
- **First-launch tutorial** — quick guide to controls

## Supported Platforms

| Platform | Watch |
|----------|-------|
| Basalt | Pebble Time / Time Steel |
| Chalk | Pebble Time Round |
| Diorite | Pebble 2 |
| Emery | Pebble Time 2 |
| Aplite | Original Pebble (no album art) |

## Setup

### 1. Deploy the Config Page

The config page handles Spotify OAuth login. You need to host it yourself:

1. Fork the [playback_config](https://github.com/alex523ap/playback_config) repo
2. Enable GitHub Pages in repo settings (deploy from `main` branch)
3. Update the [Redirect URL](https://github.com/alex523ap/Playback-for-Spotify/blob/efab8a25782437fd5ffa348742ecea22f6f1f143/src/pkjs/spotify_auth.js#L4) in your Spotify app and playback_config index.html Redirect URL to match your GitHub Pages URL

### 2. Create a Spotify App

1. Go to [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
2. Create a new app
3. Set Redirect URL to your config page URL (e.g. `https://yourusername.github.io/playback_config/`)
4. Copy your **Client ID**

### 3. Install the App

1. Clone this repo
2. Run `pebble build && pebble install`
3. Open Settings on your phone for the Playback app
4. Enter your Client ID and log in with Spotify

## Controls

| Action | Button |
|--------|--------|
| Next track | UP |
| Previous track | DOWN |
| Play / Pause | SELECT |
| Volume up | Hold UP |
| Volume down | Hold DOWN |

## Building

Requires [Pebble SDK](https://developer.rebble.io/developer.pebble.com/sdk/index.html).

```
pebble build
pebble install --emulator basalt   # emulator
pebble install --phone IP_ADDRESS  # real watch
```

## License

MIT
