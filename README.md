# ESP32-S3 e-Paper BLE Temperature Display

Firmware for the Waveshare **ESP32-S3-ePaper-1.54** that shows temperature,
humidity, battery, and signal strength from a nearby Govee BLE thermometer,
plus a 24 hour temperature curve.

The device stays awake. Deep sleep and Wi-Fi were removed: deep sleep left
the e-paper unusable, and Wi-Fi/WPS/NTP is no longer part of this firmware.

## Current behavior

1. Power on the e-paper and start a NimBLE active scan.
2. Wait up to 10 seconds for one advertisement from a device whose name
   contains `GV5171` (default: `GV5171385C`).
3. Decode temperature / humidity / battery from the Govee manufacturer data.
4. Refresh the 200x200 panel (full refresh on cycle 0 and every 15 cycles,
   partial refresh otherwise). A partial refresh is skipped when the new
   frame is identical to the one on screen.
5. Pause 5 seconds and repeat. Last known values are kept if no new packet
   arrives.

## Layouts

- **Trend** (default): temperature, 24 h maximum and minimum, the 24 h curve
  in 10 minute steps, humidity and battery. The history lives in RAM and
  starts empty after every reboot; gaps without readings stay empty.
- **Focus**: one big temperature (72 px digits), signal bars, humidity and
  battery.

Placeholders (`--`) are shown until the first reading arrives.

## BOOT button

- **Short press**: rotate the UI by another 90 degrees clockwise. The
  default orientation is 90 degrees counter-clockwise.
- **Long press** (1 s): switch between the trend and focus layouts. It
  fires while the button is still held.

Both redraw with a full refresh. A press during the 5 s pause is shown at
once; a press during a BLE scan is shown when the scan ends. Orientation
and layout are stored in NVS and survive a reboot.

The previous-frame buffer lives in static RAM, not on the main task stack.
A 5 kB stack-local copy overflowed the 8 kB main task and rebooted the
chip before any refresh could run.

## Hardware

- Board: Waveshare ESP32-S3-ePaper-1.54 (this unit is **V2**)
- Panel: 200x200 black/white e-paper (GDEY0154D67 / SSD1680)
- Sensor: Govee thermometer advertised as `GV5171385C` (prefix `GV5171`)

No extra wiring is required. On-board connections:

- EPD BUSY: GPIO 8
- EPD RST: GPIO 9
- EPD DC: GPIO 10
- EPD CS: GPIO 11
- EPD SCLK: GPIO 12
- EPD MOSI: GPIO 13
- EPD PWR (active low on V2): GPIO 6
- VBAT latch (active high): GPIO 17
- BOOT button (active low; rotation and layout): GPIO 0

## Project layout

```
epaper-display/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv              # 3 MB factory app partition
├── main/
│   ├── main.c                  # always-on scan / render loop
│   ├── board.c/.h              # GPIO + SPI2
│   ├── button.c/.h             # debounced BOOT button
│   ├── epd_ssd1680.c/.h        # e-paper driver
│   ├── display_ui.c/.h         # LVGL 9 UI + 1bpp framebuffer
│   ├── ble_scanner.c/.h        # NimBLE one-shot scan + ad counter
│   ├── govee_decode.c/.h       # Govee advertisement decoder
│   ├── history.c/.h            # 24 h temperature ring buffer
│   ├── assets/                 # 1-bit icon, 72 px digit font
│   └── Kconfig.projbuild
├── scripts/
│   ├── setup.sh
│   ├── build.sh
│   ├── flash.sh
│   ├── monitor.sh
│   ├── gen_icons.py            # regenerates assets/icon_*.h
│   ├── gen_font.py             # regenerates assets/font_digits_72.c
│   └── test_govee.sh           # host tests
└── test/
    ├── test_govee_decode.c
    └── test_history.c
```

## Setup

Needs **ESP-IDF v5.5.2** or newer. The setup script installs it to
`~/esp/esp-idf` if missing.

```bash
./scripts/setup.sh          # one-time, ~1 GB download
```

## Build

```bash
./scripts/build.sh
```

Or, with the ESP-IDF environment already sourced:

```bash
idf.py build
```

## Flash and monitor

Plug the board in with USB-C. If it is not detected, hold **BOOT** while
pressing **PWR**.

```bash
./scripts/flash.sh          # auto-detects port and flashes
./scripts/flash.sh monitor  # flash, then open the serial monitor
./scripts/monitor.sh        # serial monitor only
```

To force a port:

```bash
./scripts/flash.sh /dev/cu.usbmodemXXXX
idf.py -p /dev/cu.usbmodemXXXX flash
```

On a successful cycle the log looks like:

```
I (...) main: starting BLE scanner (cycle 4)
I (...) ble: target sensor found: GV5171385C
I (...) ui: screen updated: 24.84 C / 41.8 % / 81% batt (partial=1) footer='343 ads'
```

## Host testing

The Govee decoder and the history buffer can be tested on the host without
the ESP toolchain (binaries go to `build_host/`):

```bash
./scripts/test_govee.sh
```

## Configuration

See `main/Kconfig.projbuild` and `sdkconfig.defaults`:

- `CONFIG_APP_SENSOR_NAME_PREFIX` — default `GV5171`
- `CONFIG_APP_FULL_REFRESH_EVERY` — default `15`
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE` — `8192` (do not put the previous
  framebuffer on this stack)
- Custom partition table `partitions.csv` (3 MB factory app)
- LVGL Montserrat 12/14/16/18/20/48. An existing `sdkconfig` keeps its old
  font settings; delete it once so `sdkconfig.defaults` is applied again
  (the build stops with a clear `#error` otherwise).

## Firmware compatibility

PSRAM is disabled and the image header is 4 MB, so the same binary works
on V1 (4 MB flash + 2 MB quad PSRAM) and V2 (8 MB flash + 8 MB octal PSRAM).

## License

MIT. External code is limited to ESP-IDF / LVGL components fetched at
build time.
