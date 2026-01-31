# Instrukcje dla Claude - ESP32 Audio Player

## Build Firmware

### Zmienne środowiskowe (wymagane)
```batch
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.2
set ADF_PATH=C:\Espressif\frameworks\esp-adf
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env
set MSYSTEM=
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;%PATH%
```

### Komenda build (zwykly) - ZALECANA
```bash
cd /c/Projekty/ESP32_Audio && ./build_firmware.bat > build_log.txt 2>&1
```
Lub pelna komenda:
```bash
cmd.exe /c "set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.2&& set ADF_PATH=C:\Espressif\frameworks\esp-adf&& set IDF_TOOLS_PATH=C:\Espressif&& set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env&& set MSYSTEM=&& set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;%PATH%&& cd /d C:\Projekty\ESP32_Audio&& C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build > C:\Projekty\ESP32_Audio\build_log.txt 2>&1"
```

### Komenda reconfigure (po zmianie sdkconfig)
```bash
cmd.exe /c "set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.2&& set ADF_PATH=C:\Espressif\frameworks\esp-adf&& set IDF_TOOLS_PATH=C:\Espressif&& set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env&& set MSYSTEM=&& set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;%PATH%&& cd /d C:\Projekty\ESP32_Audio&& C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py reconfigure && C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build > C:\Projekty\ESP32_Audio\build_log.txt 2>&1"
```

### Komenda fullclean (gdy build sie psuje lub ninja nie wykrywa zmian w plikach)
```bash
rm -rf /c/Projekty/ESP32_Audio/build && cd /c/Projekty/ESP32_Audio && ./build_firmware.bat > build_log.txt 2>&1
```
**UWAGA:** Jezeli ninja nie rekompiluje zmienionych plikow .c, uzyj fullclean! Usuwanie pojedynczych .obj czasem nie dziala.

## Upload OTA

### Adres IP urzadzenia
Aktualny: `192.168.0.26`

### Komenda OTA upload (surowe dane binarne - WAZNE!)
```bash
curl -X POST --data-binary "@C:/Projekty/ESP32_Audio/build/esp32_audio_player.bin" -H "Content-Type: application/octet-stream" http://192.168.0.26/api/ota/upload --connect-timeout 30 -m 600 2>&1
```

**UWAGA:** NIE uzywaj `-F` (multipart form) - handler OTA oczekuje surowych danych binarnych!

### Sprawdzenie statusu po OTA
```bash
sleep 15 && curl -s http://192.168.0.26/api/status --connect-timeout 10 2>&1
```

## Flash przez USB (COM4)
```bash
cmd.exe /c "set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;%PATH%&& cd /d C:\Projekty\ESP32_Audio&& python -m esptool --chip esp32 -p COM4 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 40m 0x1000 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0xe000 build\ota_data_initial.bin 0x20000 build\esp32_audio_player.bin"
```

## Sprawdzanie logow urzadzenia
Monitor szeregowy uruchomiony w tle: sprawdz `b7d14f8.output`
```bash
tail -100 "C:\Users\Win10\AppData\Local\Temp\claude\C--Projekty-ESP32-Audio\tasks\b7d14f8.output"
```

## Wazne pliki
- `main/audio_player.c` - konfiguracja buforow i priorytetow
- `sdkconfig` - konfiguracja ESP-IDF (PSRAM, WiFi, itp.)
- `build_log.txt` - logi ostatniego builda

## Hardware
- Plytka: ESP32-LyraT V4.3
- PSRAM: 4MB
- Flash: 4MB
- Kodek audio: ES8388

## Aktualna konfiguracja audio (audio_player.c)
- HTTP stream: 64KB buffer, priorytet 8, core 0
- Decoder MP3: priorytet 8, core 0
- I2S stream: 32KB buffer, priorytet 10, core 1
- Pipeline buffer: 32KB
- WiFi power save: wylaczony (WIFI_PS_NONE) w wifi_manager.c
- NVS save: debounced (1s delay) w audio_settings.c

## Znane problemy

### NVS save blokuje audio (NAPRAWIONE)
Problem: Zapis do flash (NVS) trwal kilkanascie ms i blokowal CPU, powodujac mikro-przerwy podczas przesuwania suwaka glosnosci.

Rozwiazanie: Zaimplementowano debounced save w `audio_settings.c`:
- `audio_settings_save()` - teraz uzywany timer FreeRTOS, zapis nastepuje po 1s od ostatniej zmiany
- `audio_settings_flush()` - natychmiastowy zapis, uzywac przed shutdown
- Zmiany w pamięci są natychmiastowe, tylko zapis do flash jest opóźniony

### ESP-ADF vs ESP-IDF v5.5 - PSRAM stack
ESP-ADF uzywa starej nazwy opcji `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`, ale w ESP-IDF v5.5 zostala przemianowana na `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`.

Przez to ESP-ADF wyswietla ostrzezenie:
```
W AUDIO_THREAD: Make sure selected CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
```

Rozwiazanie: Stosy taskow audio sa alokowane w wewnetrznej pamieci RAM, ale glowne bufory audio (256KB+128KB+128KB) sa w PSRAM. To wystarczajace dla plynnego odtwarzania.

### Mikro-przerwy w audio
Mozliwe przyczyny i rozwiazania:
1. WiFi power save - wylaczony w wifi_manager.c po polaczeniu
2. Male bufory - zwiekszone do 256KB HTTP, 128KB I2S, 128KB pipeline
3. Priorytety taskow - HTTP/decoder=8, I2S=10, rozdzielone na 2 rdzenie
4. Siec WiFi - sprawdz RSSI (powinno byc > -70 dBm)
