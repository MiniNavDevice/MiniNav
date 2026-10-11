# MiniNav

A tiny Android Auto screen for your bike or car. MiniNav runs on the Waveshare ESP32-S3 2.8"
touch display, connects to your phone over WiFi and shows Google Maps from Android Auto.
No app on the phone, no Bluetooth, no extra hardware.

**Step-by-step guide with diagrams: https://mininavdevice.github.io/MiniNav/**
(hardware, one-click flashing, phone setup, connecting, riding, printing the case, troubleshooting).
Building from source has its own page: [docs/build-from-source.html](docs/build-from-source.html).

## How it works

Android Auto runs on the phone: the phone draws the Maps screen and streams it as video to a car's
screen (the head unit), which only displays it. MiniNav plays the part of that car screen. Android
Auto has a hidden developer option, **Start headunit server**, that lets a head unit connect to the
phone over WiFi (TCP 5277); MiniNav connects, completes the Android Auto handshake (TLS), receives the
H.264 map video and decodes it on the ESP32-S3.

- Joins your phone's hotspot (recommended, the phone keeps its mobile data) or runs its own WiFi.
- Day/night themes follow Android Auto's night mode.
- Optional full screen: hides Android Auto's app bar so the map fills the display.
- Optional touch: taps go to Android Auto (launcher, app lists).
- Built-in demo drive that works without a phone.
- Flips the picture when the board is turned over.

## Hardware

| Item | Notes |
|---|---|
| Waveshare ESP32-S3-Touch-LCD-2.8 | ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 320x240 ST7789 LCD, CST3530 touch, QMI8658 IMU. [Buy on Robu.in](https://robu.in/product/waveshare-esp32-s3-2-8inch-capacitive-touch-display-development-board-240x320-ips-5-point-touch-32-bit-lx7-dual-core-processor-supports-wifi-bluetooth-esp32-with-display/) |
| USB-C data cable | Flashing and power |
| Android phone | Android Auto installed |
| 3D-printed case | Optional: front frame with BOOT button + back panel with strap slits, in `docs/case/`. No printer? Printing services such as [Robu.in](https://robu.in/product/3d-printing-service1/) print it for about ₹100-200 |

## Flash (easy way)

Open the install guide in Chrome or Edge, either online at **https://mininavdevice.github.io/MiniNav/**
or `docs/index.html` from a clone, plug the board in and click **Install MiniNav**. Nothing to install. The firmware file is
`docs/flash/MiniNav.bin` (one image for address **0x0**, not the 0x1000 some flashers default to) if you prefer another flasher.

## Build from source

Only needed to change WiFi names and passwords or other options.

Needs [ESP-IDF v5.4.2](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/get-started/).

```sh
git clone https://github.com/MiniNavDevice/MiniNav.git
cd MiniNav
tools/fetch_lovyangfx.sh            # display library, once
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p <port> -b 921600 flash    # e.g. /dev/cu.usbmodem101, /dev/ttyACM0, COM5
```

If flashing can't connect: hold BOOT, tap RESET, release BOOT, then flash again.

After changing the code, `python3 tools/make_web_firmware.py` refreshes the files the guide's Install
button flashes (`docs/flash/MiniNav.bin` and `MiniNav.bin.js`).

## Phone setup (once)

1. Android Auto settings: tap **Version** 10 times to unlock developer settings.
2. Three-dot menu → **Start headunit server**.
3. Android Auto battery usage → **Unrestricted**.
4. Hotspot: name **MiniNavHotspot**, password **mininav123**, 2.4 GHz.
   (Or join the board's own WiFi **MiniNav**, password **mininav123**.)

Change names and passwords with `idf.py menuconfig` → *MiniNav (Android Auto minimap)*.

## Controls

| Do | Result |
|---|---|
| Tap Touch / Full screen / Theme on the connection screen | Change settings (saved) |
| Press BOOT | Day / night theme |
| Double-press BOOT | Touch on / off |
| Hold BOOT 1 s | Demo drive on / off |

## Options (`idf.py menuconfig`)

| Option | Default |
|---|---|
| Access point name / password / channel | MiniNav / mininav123 / 6 |
| Phone hotspot name / password | MiniNavHotspot / mininav123 |
| Video density (bigger labels when higher) | 200 dpi |
| Hide Android Auto's app bar | on |
| Right-hand drive | on |

## Known issues

- Touch reaches Android Auto's launcher and lists, but not the Maps view itself.
- If the screen sticks at "Version", restart the phone's headunit server
  (three-dot menu → Stop, then Start headunit server).

## Code layout

```
main/app_main.cpp   start-up, inputs, serial console
main/board.cpp      display, connection screen, BOOT button, touch, IMU
main/net.cpp        WiFi (access point + phone hotspot)
main/aap.cpp        Android Auto protocol: framing, TLS, channels, input
main/video.cpp      H.264 decoder, crop and scale to the LCD
main/demo.cpp       built-in demo clip (main/demo.h264)
tools/              helper scripts (display library fetch, dev tools)
docs/               the guide website (GitHub Pages): web flasher (flash/), case STLs (case/)
```

## Credits and license

- Android Auto protocol details follow Open Headunit;
  `main/aap_cert.h` is its published reference head-unit certificate.
- Display: [LovyanGFX](https://github.com/lovyan03/LovyanGFX). Decoder: Espressif `esp_h264`.
- UI font: Nunito (SIL Open Font License, `tools/fonts/OFL.txt`).
- MiniNav is licensed under the GNU AGPL-3.0 (`LICENSE`).

Not affiliated with Google. Android Auto is a trademark of Google LLC.
