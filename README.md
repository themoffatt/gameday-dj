# gameday-dj

Firmware for an ESP32-based GameDay DJ device.

## Hardware behavior

- Two push-buttons trigger playback of MP3 files from the SD card.
- Bluetooth audio is sent to a paired speaker.
- A status LED turns on when the ESP32 reports an active Bluetooth speaker connection.
- The physical on/off switch controls power to the device.
- The LiPo charging circuit and USB-C charging port are hardware-managed and do not require firmware control.

## Firmware file

- `gameday_dj.ino`

## SD card files

Place these files at the root of the SD card:

- `button1.mp3`
- `button2.mp3`

## Default pin mapping

- Button 1: GPIO 25 (active-low, internal pull-up enabled)
- Button 2: GPIO 26 (active-low, internal pull-up enabled)
- Bluetooth status LED: GPIO 2
- SD card CS: GPIO 5
