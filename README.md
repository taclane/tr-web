<img src="https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/trunk-recorder-header.png" width="75%" height="75%">

[![Discord](https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/discord.jpg)](https://discord.gg/btJAhESnks) &nbsp;&nbsp;

# Trunk Recorder Web Status Plugin <!-- omit from toc -->

A web dashboard for Trunk Recorder. It shows live calls, recorders, systems, the console and unit affiliations, and keeps call and affiliation history in a local database.

Requires Trunk Recorder 5.0 or later. It needs no other libraries: SQLite and the browser libraries are included in the source.

- [Features](#features)
- [Install](#install)
- [Configure](#configure)
  - [Plugin Options](#plugin-options)
  - [Authentication](#authentication)
  - [Restart Button](#restart-button)
  - [Data Storage](#data-storage)
  - [HTTPS](#https)
- [Dashboard](#dashboard)
  - [Bit Error Rate](#bit-error-rate)
- [API](#api)

## Features

- Live updates over Server-Sent Events
- Active and recent calls, recorders, devices and control channels
- Decode rate and active call charts (5, 15 and 60 minutes)
- Per-system channel quality, busiest talkgroups, and the talkgroups and radios with the most decoder errors, since restart or over 24 hours, 7 days, 30 days or all time
- Unit and talkgroup affiliation history, with JSON export
- Gephi graph stream of unit and talkgroup activity
- Live console
- Admin page: config editor, restart, login history
- Two access levels (info and admin), login sessions that survive restarts
- HTTPS
- Themes: Nostromo, Classic, Hot Dog Stand

## Install

1. **Clone Trunk Recorder** source following these [instructions](https://github.com/robotastic/trunk-recorder/blob/master/docs/Install/INSTALL-LINUX.md).

2. **Clone this plugin** into the `user_plugins` directory. Trunk Recorder builds and installs it with the rest of the project.

```bash
cd [your trunk-recorder source directory]
cd user_plugins
git clone https://github.com/taclane/tr-web.git
cd [your trunk-recorder build directory]
cmake ..
make
sudo make install
```

To update, run `git pull` in `user_plugins/tr-web` and rebuild.

## Configure

Add the plugin to Trunk Recorder's `config.json`:

```json
{
  "plugins": [
    {
      "library": "libtr_web_plugin.so",
      "name": "tr-web",
      "port": 8080,
      "admin_username": "trunkadmin",
      "admin_password": "admintrunk"
    }
  ]
}
```

### Plugin Options

| Key | Required | Default Value | Type | Description |
| --- | :------: | ------------- | ---- | ----------- |
| port | | 8080 | integer | HTTP or HTTPS port |
| bind | | `"0.0.0.0"` | string | Bind address. `0.0.0.0` listens on all interfaces. |
| username | | `""` | string | Info-level username. Empty disables info-level login. |
| password | | `""` | string | Info-level password |
| admin_username | | `""` | string | Admin-level username |
| admin_password | | `""` | string | Admin-level password |
| ssl_cert | | `""` | string | Certificate PEM file. Enables HTTPS with `ssl_key`. |
| ssl_key | | `""` | string | Private key PEM file |
| console_lines | | 5000 | integer | Console lines kept for the Console tab |
| max_connections | | 64 | integer | Simultaneous connections (page loads, API calls and live streams). Extra connections are refused. |
| theme | | `"nostromo"` | string | Default theme: `nostromo`, `classic` or `hotdog` |
| affiliation_timeout | | 12 | integer | Hours without activity before a unit or talkgroup shows as idle |
| database | | `"tr-web.db"` | string | Database file. Relative paths start from Trunk Recorder's working directory. See [Data Storage](#data-storage). |
| affiliation_cache | | `"affiliations.json"` | string | Affiliation file from tr-web versions without a database. See [Data Storage](#data-storage). |
| affiliation_export | | `""` | string | Also write the affiliation history to this JSON file. tr-web never reads it. |
| affiliation_export_interval | | 3600 | integer | Seconds between `affiliation_export` writes (minimum 60) |
| trusted_proxies | | `["127.0.0.1"]` | array | Reverse proxy addresses. tr-web takes the client address from `X-Forwarded-For` or `X-Real-IP` only on connections from these addresses. |

### Authentication

There are two access levels:

- **Info** (`username`/`password`): status pages, calls, console and affiliations
- **Admin** (`admin_username`/`admin_password`): everything, including the config editor and restart

The configured credentials decide what needs a login:

| Configured | Status pages | Admin features |
| ---------- | ------------ | -------------- |
| nothing | open | **open to anyone who can connect** |
| info only | info login | **info login** |
| admin only | open | admin login |
| info and admin | info or admin login | admin login |

Set `admin_username` and `admin_password` if anyone you don't trust can reach the dashboard. tr-web logs a warning at startup when admin features are not protected by admin credentials. `/health` never needs a login.

tr-web allows 10 wrong passwords per minute from one client address. Requests without credentials don't count.

### Restart Button

The admin **Restart** button stops Trunk Recorder gracefully (SIGINT, the same as Ctrl+C). It concludes active calls and stops plugins before it exits. Something else must start it again:

- **systemd:** `Restart=always` in the service unit. `Restart=on-failure` does not restart after a clean exit.
- **Docker:** a `restart: always` or `restart: unless-stopped` policy
- **Run by hand:** Trunk Recorder exits.

Saving in the config editor keeps the previous file as `config.json.bak.trweb`. Both files keep the original's permissions. A `config.json` bind-mounted into a container is rewritten in place.

### Data Storage

tr-web stores its history in a SQLite database, `tr-web.db` by default:

- every unit and talkgroup seen, and how often each unit used each talkgroup
- login sessions, so a restart doesn't log users out (stored as hashes of the session tokens; changing a username or password ends all sessions)
- every login attempt (the admin page shows the last 50)
- hourly call statistics for each channel, talkgroup and radio
- decode rate and active call samples: the last 2 hours in full, so the charts survive a restart, and per-minute averages, minimums and maximums permanently

tr-web creates the file readable by its owner only, writes changes every 5 seconds and syncs them to disk. It copies the database to `tr-web.db.bak` once a day.

If the database can't be opened or fails its integrity check, tr-web leaves the file untouched. It logs an error, keeps the history in memory and writes it to `tr-web.db.fallback.json` every 5 minutes and at shutdown.

**Upgrading from a version without a database:** tr-web imports `affiliations.json` the first time it starts with an empty database, then renames it to `affiliations.json.imported`. To go back to the older version, rename it back to `affiliations.json`. Activity recorded since the upgrade exists only in the database. The old `affiliation_autosave` option is no longer used.

### HTTPS

Create a self-signed certificate:

```bash
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes
```

Set the paths in the plugin options:

```json
{
  "ssl_cert": "/path/to/cert.pem",
  "ssl_key": "/path/to/key.pem"
}
```

## Dashboard

Open `http://your-server:8080` (or `https://` with HTTPS configured).

- **Status:** active and recent calls, recorders, decode rate and call charts, devices
- **Recorders / Devices:** recorder and SDR details
- **Systems:** per system:
  - decode rate and active call charts
  - **Channels:** calls and bit error rate per channel
  - **Talkgroups:** busiest talkgroups
  - **Top errors:** the talkgroups or radios with the most decoder errors
  - **Site:** system IDs, control channels and file names
  - the talkgroup list, unit tags and over-the-air aliases
  - a details panel with hourly calls and bit error rate for the system, or for the selected channel, talkgroup or radio
- **Console:** Trunk Recorder's log, with ANSI colors and filtering
- **Omnitrunker:** trunking messages (grants, affiliations, registrations)
- **Affiliations:** units and talkgroups with their activity, and JSON export. Names follow Trunk Recorder's current unit tag or talkgroup alias (`unitTagsMode` decides between user tags and over-the-air aliases); a unit keeps its last name until a new one is found.
- **Admin:** login history, config editor, restart

### Bit Error Rate

The Systems tab shows decoder errors as a bit error rate: the share of received voice bits that the decoder's error correction repaired. P25 Phase 1 carries 7,200 coded voice bits per second. Phase 2 and DMR carry 3,600 per slot. Under 1% sounds clean, 1-2% is audible, and over 2% is degraded.

Where errors collect shows where to look:

- **Channel:** errors on one channel, whatever talkgroups and radios use it, point at the receiving site's setup or at interference.
- **Talkgroup or radio:** errors that follow a talkgroup or radio were already in the signal as received (portables in buildings, units in tunnels or moving fast between sites).

Trunk Recorder currently reports errors for P25 Phase 1 audio only. Phase 2 and DMR audio show 0%. Analog audio has no bit error rate.

## API

All `/api` endpoints return JSON. Endpoints that need a login accept the session cookie set by `/api/login`, an `Authorization: Bearer <token>` header with the token it returns, or HTTP Basic authentication.

### Public

| Endpoint | Method | Description |
| -------- | ------ | ----------- |
| `/` | GET | Dashboard |
| `/health` | GET | Health check |
| `/api/login` | POST | `{"username", "password"}`; returns a session token and sets the session cookie |
| `/api/logout` | POST | Ends the session |

### Info Level

| Endpoint | Method | Description |
| -------- | ------ | ----------- |
| `/api/status` | GET | Recorders, calls, systems, devices, rates, chart history, recent calls and console |
| `/api/whoami` | GET | Access level and username |
| `/api/rates/history` | GET | Decode rate history |
| `/api/calls/rate-history` | GET | Active call history |
| `/api/console` | GET | Console lines |
| `/api/affiliations` | GET | Units and talkgroups. `view=units` or `talkgroups`, `since=<server_time>` for changes only, `limit=N`. |
| `/api/system/stats?sys_num=N` | GET | Channel, talkgroup and radio statistics. `window=restart` (default), `24h`, `7d`, `30d` or `all`. |
| `/api/system/history?sys_num=N` | GET | Hourly history. `kind=system`, or `freq`, `talkgroup` or `unit` with `id=X`; `window` as above. |
| `/api/system/talkgroups?sys_num=N` | GET | Talkgroup list |
| `/api/system/unit_tags?sys_num=N` | GET | Unit tags |
| `/api/system/unit_tags_ota?sys_num=N` | GET | Over-the-air unit aliases |
| `/events` | GET | Server-Sent Events stream |
| `/graph-stream` | GET | Gephi graph stream (JSON lines) |

### Admin Level

| Endpoint | Method | Description |
| -------- | ------ | ----------- |
| `/api/admin/config` | GET | Trunk Recorder's config file |
| `/api/admin/save-config` | POST | `{"content"}`; saves the config file and keeps a backup |
| `/api/admin/restart` | POST | Stops Trunk Recorder gracefully (see [Restart Button](#restart-button)) |
| `/api/admin/login-history` | GET | Last 50 login attempts |

### Server-Sent Events

| Event | Description |
| ----- | ----------- |
| `calls` | Active calls |
| `call_start` | A call started |
| `call_end` | A call ended, with Trunk Recorder's call metadata |
| `recorders` | Recorder status |
| `systems` | System list |
| `rates` | Decode rates |
| `devices` | SDR devices |
| `unit_event` | Grant, affiliation, registration and other unit messages |
| `console_batch` | New console lines |
| `event_drop` | Number of events dropped while the client was slow |
| `server_shutdown` | Trunk Recorder is stopping |
