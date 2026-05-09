# Strava Exercise Load Monitor

This project now drives a LilyGo T-Display S3 as a standalone Strava exercise-load dashboard. The firmware renders a 53-week activity grid and a compact stats view, while a small local bridge service reads the SQLite database produced by `statistics-for-strava` and exposes it as simple JSON for the device.

The app no longer depends on GitHub credentials, GitHub APIs, or GitHub contribution data.

## What It Does

- Displays 53 weeks of daily exercise load on the T-Display S3.
- Rotates between a heatmap screen and a stats screen.
- Hosts a browser-based config page for WiFi, bridge URL, brightness, animation, orientation, and MQTT topics.
- Uses NVS so device settings survive reboots.
- Supports an AP-based first-boot flow when WiFi is not configured.
- Optionally exposes brightness control over MQTT.

## Architecture

The project has two parts:

1. Firmware in `src/` built with PlatformIO and Arduino for the LilyGo T-Display S3.
2. A lightweight Python bridge in `strava_bridge.py` that reads the Strava statistics SQLite database and serves `GET /api/exercise-load`.

The device fetches exercise data from the bridge over HTTP, not directly from Strava.

## Hardware Target

| Component | Details |
|-----------|---------|
| Board | LilyGo T-Display S3 |
| MCU | ESP32-S3 |
| Display | Integrated ST7789V 170x320 LCD |
| Framework | Arduino via PlatformIO |
| UI | LVGL v9 |

The default PlatformIO environment is `lilygo-t-display-s3`.

## Data Contract

The bridge responds with JSON shaped like this:

```json
{
  "generated_at": "2026-05-08T12:00:00",
  "weeks": 53,
  "days": [
    {"date": "2025-06-01", "load": 45.5}
  ],
  "total_load": 12345.0,
  "total_activities": 156,
  "busiest_date": "2025-08-15",
  "busiest_load": 285.3
}
```

`load` is daily moving time in minutes.

## Quick Start

### 1. Prepare local secrets

Create the firmware fallback WiFi header from the example:

```powershell
Copy-Item include\secrets.h.example include\secrets.h
```

Edit `include/secrets.h` with your WiFi details:

```c
#define WIFI_SSID     "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
```

If you keep local-only notes for deployment hosts, paths, or URLs, copy the text template too:

```powershell
Copy-Item secrets\secrets.example.txt secrets\secrets.txt
```

Both real files are ignored by Git.

### 2. Start the Strava bridge

Run it directly:

```powershell
python strava_bridge.py --db C:\path\to\your\strava.sqlite3 --port 8082
```

Or build and run it with Docker after adjusting the volume mount in `docker-compose.strava-bridge.yml`:

```powershell
docker compose -f docker-compose.strava-bridge.yml up -d --build
```

The firmware expects a bridge URL like:

```text
http://192.168.1.54:8082/api/exercise-load
```

### 3. Build and flash the firmware

```powershell
pio run -e lilygo-t-display-s3 -t upload
```

To open the serial monitor:

```powershell
pio device monitor -b 115200
```

### 4. Configure the device

On first boot, if no WiFi is stored in NVS, the device starts an access point named `StravaMonitor-Setup`.

1. Connect to `StravaMonitor-Setup`.
2. Open `http://192.168.4.1`.
3. Enter WiFi credentials and the bridge URL.
4. Save and reboot.

Once the device joins your network, the config page is available at `http://<device-ip>/`.

## Web Configuration Fields

| Field | NVS Key | Description |
|------|---------|-------------|
| WiFi SSID | `wifi_ssid` | Network name |
| WiFi Password | `wifi_pass` | Network password |
| Bridge server URL | `srv_url` | Full `strava_bridge.py` endpoint URL |
| LCD brightness | `brightness` | Display brightness from 0-100 |
| Screen switch interval | `switch_sec` | Grid/stats rotation interval |
| Data refresh interval | `refresh_min` | How often the device refetches data |
| Flip screen | `flip_scr` | 180-degree rotation toggle |
| Animation top percent | `anim_pct` | Top percentage of active days that breathe |
| Animation period | `anim_ms` | Breathing animation period in ms |
| LED brightness | `rgb_bright` | Optional RGB brightness from 0-100 |
| Min breath period | `rgb_pmin` | Fastest LED breath period |
| Max breath period | `rgb_pmax` | Slowest LED breath period |
| Load target | `rgb_smax` | Daily minutes that hit max LED speed |
| MQTT broker | `mqtt_host` | Broker host or IP |
| MQTT port | `mqtt_port` | Broker port |
| Combined brightness topic | `mqtt_ctopic` | Sets LCD and LED brightness |
| LCD brightness topic | `mqtt_lcd` | LCD-only brightness topic |
| LED brightness topic | `mqtt_ltopic` | LED-only brightness topic |

Brightness, rotation, and LED settings apply immediately. WiFi and bridge URL changes require a reboot.

## Bridge Deployment Notes

`deploy.ps1` is a helper for copying the bridge files to a remote Docker host and restarting the container. It assumes SSH access and Docker Compose are already available on the target machine.

Before using it, check these values:

- `RemoteHost`
- `RemoteDir`
- The bind mount in `docker-compose.strava-bridge.yml`

Those values are environment-specific and should stay out of tracked commits.

## Testing

Run the bridge tests with:

```powershell
python -m unittest tests.test_strava_bridge
```

The test suite:

- Creates a temporary SQLite database.
- Verifies aggregate calculations.
- Starts a local ephemeral HTTP server and checks the API response.
- Optionally checks a remote bridge endpoint using `STRAVA_BRIDGE_REMOTE_URL`.

## Sensitive Files

These files are intended to remain local only:

- `include/secrets.h`
- `secrets/secrets.txt`

Committed templates are provided here instead:

- `include/secrets.h.example`
- `secrets/secrets.example.txt`

If a real secret file was previously added to Git, ignoring it is not enough by itself. Remove it from the index with `git rm --cached <path>` and then commit the ignore/template changes.

## Project Layout

```text
include/
  secrets.h.example        WiFi fallback template
src/
  main.cpp                 Device setup, fetch loop, screen orchestration
  config.*                 NVS-backed device settings
  strava_api.*             HTTP fetch and JSON parsing for bridge data
  display_grid.*           53-week grid screen
  display_stats.*          Stats screen
  web_server.*             Browser-based config UI
  mqtt_client.*            Optional MQTT brightness integration
strava_bridge.py           SQLite to JSON bridge service
docker-compose.strava-bridge.yml
deploy.ps1                 Optional remote bridge deployment helper
tests/test_strava_bridge.py
```

## Troubleshooting

**Blank screen after flash**

- Confirm you built `lilygo-t-display-s3`.
- Check serial output at 115200 baud.
- Verify the display powers up and the board is not browning out on USB.

**Device stuck in setup mode**

- Save valid WiFi credentials through the config page.
- If you want a compile-time fallback, make sure `include/secrets.h` exists.

**No activity data appears**

- Verify the bridge URL from a browser first.
- Check that the bridge can read the SQLite database.
- Review serial logs for fetch failures and HTTP status codes.

**Web UI not reachable after WiFi join**

- Look up the device IP from serial output or DHCP leases.
- Ensure the laptop and device are on the same network.

## License

MIT. See `LICENSE`.
