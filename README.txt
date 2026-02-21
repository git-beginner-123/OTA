Build:
- idf.py set-target esp32s3
- idf.py build flash monitor

WiFi OTA upgrade:
- Run `idf.py menuconfig`
- Set `COMM WiFi -> WiFi SSID/WiFi Password`
- Set `COMM WiFi -> OTA firmware URL` (for example `http://<pc-ip>:8000/stem_framework_idf61_lcd.bin`)
- Build and flash once by USB
- In device menu open `WIFI OTA`, press `OK` to download, write OTA partition, and auto reboot

Display ST7735:
SCK=GPIO21 MOSI=GPIO47 CS=GPIO41 DC=GPIO40 RST=GPIO45 BLK=GPIO42

Input UART1 frame:
GPIO35/36, frame AA CMD 55
CMD 01..05, CMD05 treated as Enter

SOLAR MJPEG stable profile:
- See `docs/SOLAR_MJPEG.md` for fixed transcode/playback/transfer/flash settings.
