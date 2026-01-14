<img src="https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/trunk-recorder-header.png" width="75%" height="75%">

[![Discord](https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/discord.jpg)](https://discord.gg/btJAhESnks) &nbsp;&nbsp;

# Trunk Recorder Web Status Plugin <!-- omit from toc -->

Web-based dashboard for monitoring trunk-recorder status in real-time with live updates, charts, and administrative controls.

- [Features](#features)
- [Install](#install)
- [Configure](#configure)
  - [Plugin Options](#plugin-options)
- [Usage](#usage)
- [API Endpoints](#api-endpoints)

## Features

- **Real-time updates** via Server-Sent Events (SSE)
- **Active call monitoring** with talkgroup details, frequencies, and durations
- **Historical charts** for decode rates and call activity (5m, 15m, 60m views)
- **Recorder status** with detailed device information
- **System information** including control channels, talkgroups, and unit tags
- **Live console** output
- **Admin panel** with basic configuration editor and restart controls
- **Two-tier authentication** (info-level and admin-level access)
- **UI themes** (Nostromo, Classic, Hot Dog Stand)
- **HTTPS support**
- **No external dependencies**!

## Install

1. **Clone Trunk Recorder** source following these [instructions](https://github.com/robotastic/trunk-recorder/blob/master/docs/Install/INSTALL-LINUX.md).

2. **Build and install the plugin:**

&emsp; This pluigin source should be cloned into the `/user_plugins` directory of the Trunk Recorder 5.0+ source tree.  It will be built and installed along with Trunk Recorder.

```bash
cd [your trunk-recorder github source directory]
cd user_plugins
git clone [this repo]
cd [your trunk-recorder build directory]
cmake ..
make
sudo make install
```

## Configure

**Plugin Usage:**

Add the plugin to your trunk-recorder `config.json`:

```json
{
  "plugins": [
    {
      "library": "libtr_web_plugin.so",
      "name": "tr-web",
      "port": 8080,
      "bind": "0.0.0.0",
      "admin_username": "trunkadmin",
      "admin_password": "admintrunk"
    }
  ]
}
```

### Plugin Options

| Key              | Required | Default Value | Type       | Description                                                                       |
| ---------------- | :------: | ------------- | ---------- | --------------------------------------------------------------------------------- |
| port             |          | 8080          | integer    | HTTP/HTTPS port                                                                   |
| bind             |          | `"0.0.0.0"`   | string     | Bind address (0.0.0.0 = all interfaces)                                           |
| username         |          | `""`          | string     | Info-level auth username (empty = no auth)                                        |
| password         |          | `""`          | string     | Info-level auth password                                                          |
| admin_username   |          | `""`          | string     | Admin-level auth username                                                         |
| admin_password   |          | `""`          | string     | Admin-level auth password                                                         |
| ssl_cert         |          | `""`          | string     | Path to SSL certificate PEM                                                       |
| ssl_key          |          | `""`          | string     | Path to SSL private key PEM                                                       |
| console_lines    |          | 5000          | integer    | Console log buffer size                                                           |
| theme            |          | `"nostromo"`  | string     | Default UI theme (`nostromo`, `classic`, `hotdog`)                                |

### Authentication

The plugin supports two-tier authentication:

- **Info-level** (`username`/`password`): Read-only access to status, calls, and console
- **Admin-level** (`admin_username`/`admin_password`): Full access including config editor and restart

If only info-level credentials are set, all authenticated users have read-only access. If admin credentials are also set, admin features require the admin credentials. The `/health` endpoint always bypasses authentication for monitoring.

### HTTPS Setup

Generate a self-signed certificate:

```bash
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes
```

Configure paths in `config.json`:

```json
{
  "ssl_cert": "/path/to/cert.pem",
  "ssl_key": "/path/to/key.pem"
}
```

## Usage

Access the dashboard at `http://your-server:8080` (or `https://` if configured).

### Dashboard Tabs

- **Status**: Active/recent calls, recorder grid, system summary, device tiles
- **Recorders**: Detailed recorder status table
- **Systems**: Per-system info, control channels, talkgroup/unit tag data
- **Console**: Live console output with ANSI colors and filtering
- **Admin**: System status, login history, config editor, restart controls

## API Endpoints

### Public Endpoints

| Endpoint  | Method | Description                  |
| --------- | ------ | ---------------------------- |
| `/`       | GET    | Dashboard HTML               |
| `/health` | GET    | Health check (always public) |

### Info-Level Endpoints

| Endpoint                              | Method | Description                 |
| ------------------------------------- | ------ | --------------------------- |
| `/api/status`                         | GET    | Full system snapshot        |
| `/api/whoami`                         | GET    | Current user info and access|
| `/api/system/talkgroups?sys_num=N`    | GET    | Talkgroup list for system   |
| `/api/system/unit_tags?sys_num=N`     | GET    | Manual unit tags for system |
| `/api/system/unit_tags_ota?sys_num=N` | GET    | OTA unit aliases for system |
| `/events`                             | GET    | Server-Sent Events stream   |

### Admin-Level Endpoints

| Endpoint                   | Method | Description                      |
| -------------------------- | ------ | -------------------------------- |
| `/api/admin/config`        | GET    | Current config.json content      |
| `/api/admin/save-config`   | POST   | Save config (creates backup)     |
| `/api/admin/restart`       | POST   | Restart trunk-recorder via SIGHUP|
| `/api/admin/login-history` | GET    | Recent login attempts            |

### Server-Sent Events

Real-time updates via `/events`:

| Event | Description |
|-------|-------------|
| `calls` | Active calls array |
| `call_start` | New call started |
| `call_end` | Call completed (includes full metadata) |
| `recorders` | Recorder status array |
| `systems` | System status array |
| `rates` | Decode rates object |
| `call_rates` | Call rate history object |
| `devices` | Device status array |
| `console` | Console log line(s) |

### Example: /api/status Response

```json
{
  "systems": [{
    "sys_name": "CountyP25",
    "sys_num": 0,
    "type": "p25",
    "control_channel": 851000000,
    "control_channels": [851000000, 852000000]
  }],
  "recorders": [{
    "id": 0,
    "type": "P25",
    "rec_state_type": "RECORDING",
    "talkgroup": 101
  }],
  "calls": [{
    "call_num": 12345,
    "talkgroup": 101,
    "talkgroup_alpha_tag": "Fire Dispatch",
    "freq": 851025000,
    "emergency": false,
    "encrypted": false
  }],
  "callHistory": [],
  "rates": {"CountyP25": {"decoderate": 35.7}},
  "rateHistory": {"CountyP25": []},
  "callRateHistory": {"CountyP25": []},
  "devices": [{
    "driver": "rtlsdr",
    "device": "0",
    "center": 851000000,
    "rate": 2400000,
    "gain": 38.6
  }],
  "consoleLogs": [],
  "config": {"theme": "nostromo"}
}
```
