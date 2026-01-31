# ESP32 Audio Player

Internet radio player with Home Assistant integration for ESP32-LyraT V4.3.

## Features

- Internet radio streaming (MP3/AAC)
- Web interface for control
- Home Assistant integration via MQTT
- Configurable radio stations list
- Spotify Web API control (phone as source)
- Alarm clock with NTP time sync
- Touch button controls

## Hardware

- **Board:** ESP32-LyraT V4.3
- **Audio Codec:** ES8388
- **Amplifier:** 2x NS4150 (3W per channel)
- **WiFi:** External antenna (IPEX)

## Building

### Prerequisites

1. Install ESP-IDF v5.0+
2. Install ESP-ADF

### Setup

```bash
# Set environment
. $IDF_PATH/export.sh
. $ADF_PATH/export.sh

# Configure
idf.py menuconfig

# Build
idf.py build

# Flash
idf.py -p COM3 flash monitor
```

## Configuration

Copy `credentials.example.h` to `credentials.h` and fill in:
- WiFi SSID and password
- MQTT server details
- OTA password

## Web Interface

After flashing, connect to WiFi and open:
```
http://<device-ip>/
```

## MQTT Topics

| Topic | Description |
|-------|-------------|
| `homeassistant/media_player/esp32_audio/state` | Player state |
| `homeassistant/media_player/esp32_audio/set` | Commands |
| `homeassistant/media_player/esp32_audio/availability` | Online status |

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Get player status |
| `/api/play` | POST | Play URL |
| `/api/stop` | POST | Stop playback |
| `/api/pause` | POST | Pause playback |
| `/api/resume` | POST | Resume playback |
| `/api/volume` | POST | Set volume |
| `/api/stations` | GET/POST | Radio stations |
| `/api/alarms` | GET/POST | Alarms |

## Default Radio Stations

- VOX FM Poznan
- RMF FM
- Radio ZET
- Eska Rock
- Polskie Radio Trojka

## License

MIT
## when job done:
HTTP and https radios woking
alarm can start radio and play tone
search list can find radio Olsztyn
MQTT tested and working