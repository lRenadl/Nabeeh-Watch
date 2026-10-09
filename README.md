# Nabeeh Watch

Nabeeh is a smartwatch for deaf and hard-of-hearing users. It alerts the wearer to important sounds around them and shows each alert on the watch as Arabic text or as a sign-language animation.

This repository contains the watch firmware, built for the LILYGO T-Watch S3.

## Supported Alerts

| Alert | Arabic label |
|---|---|
| Doorbell | جرس الباب |
| Knocking on the door | طرق على الباب |
| Baby crying | بكاء أطفال |
| Fire alarm | إنذار حريق |
| Adhan | الأذان |

## Features

- **Arabic interface** with a splash screen, onboarding, home, settings and alert screens
- **Two alert modes**: text or sign language, chosen during onboarding and changeable in settings
- **Home screen** with the clock, Arabic date, battery level and phone connection status
- **Microphone recording** from the onboard PDM mic, saved as WAV files on the watch
- **Wi-Fi tests** for communication between the watch and a companion device

## Hardware

- LILYGO T-Watch S3 (ESP32-S3, 16MB flash, 8MB PSRAM, 240x240 display)

## Project Structure

| Path | Description |
|---|---|
| `src/main.cpp` | Current firmware entry point (microphone bring-up test) |
| `main_ui_backup.cpp` | The full watch UI: home, settings, alert and sign-language screens |
| `src/images/`, `src/fonts_bitmap/` | Icons, sign-language animations and Arabic fonts |
| `assets/fonts/` | Source font files |
| `LilyGoLib/` | LILYGO hardware library (git submodule) |
| `boards/`, `variants/` | PlatformIO board definition for the T-Watch S3 |
| `wifi_test/`, `test-scripts/` | Wi-Fi test firmware and its Python client |
| `platformio.ini` | Build configuration |

## Build and Upload

The project uses [PlatformIO](https://platformio.org/).

```bash
git clone --recursive https://github.com/lRenadl/Nabeeh-Watch.git
cd Nabeeh-Watch
pio run -e twatchs3 -t upload
```

## Technologies

- C / C++ with the Arduino framework for ESP32
- LVGL for the user interface
- PlatformIO

## Team

- Renad Alowais
- Abeer Alsahli
- Ragad Alrashid
- Ruba Alrzouq
