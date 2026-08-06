# PayBox — ESP32-S3 PromptPay Payment Terminal

Firmware for a self-service PromptPay payment box built on the **JC3248W535EN**
board (ESP32-S3 + 3.5" 480×320 capacitive touch display). The customer enters an
amount on screen, the device requests a PromptPay QR from a backend, displays it
with a 2-minute countdown, and polls for payment confirmation.

This is the **dark UI revision** of the original PayBox firmware — same payment
logic and backend contract, redesigned customer-facing screens.

---

## Hardware

| | |
|---|---|
| Board | JC3248W535EN (ESP32-S3-WROOM-1 **N16R8**) |
| Flash / PSRAM | 16 MB QIO / 8 MB OPI |
| Display | 3.5" 320×480 AXS15231B, QSPI — rotated 90° → **480×320 landscape** |
| Touch | AXS15231B capacitive, I²C |
| USB | native ESP32-S3 USB CDC (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`) |

## Toolchain

PlatformIO (VS Code extension or `pio` CLI). Everything else is pulled in
automatically on first build.

- `platform = espressif32` (official) → Arduino core 2.0.17 / ESP-IDF 4.4
- LVGL 8.3.x and ArduinoJson 6.21.3 via `lib_deps`
- Custom board definition in `boards/esp32-s3-n16r8v.json`

## Build and flash

```bash
pio run                       # build
pio run -t upload             # build + flash
pio device monitor            # serial monitor @ 115200
pio run -t upload -t monitor  # all three
```

### Windows

The board enumerates as a native USB CDC device (VID `0x303A`, PID `0x1001`) and
appears as a **COM port** — Windows 10/11 supply the driver automatically. List
ports with:

```powershell
pio device list
```

If the port never appears:

1. **Use a data-capable USB cable.** A charge-only cable powers the board (screen
   lights up) but never enumerates. This is the single most common failure.
2. Plug directly into the PC, not through a USB hub.
3. Force ROM download mode: hold **BOOT**, tap **RESET**, release **BOOT**. Native
   USB is created by the running firmware, so a crashed app means no port at all —
   ROM download mode enumerates regardless.
4. If a `1A86:7523` device shows up instead, the board has a CH340 bridge and needs
   the WCH CH341SER driver.

### macOS / Linux

Same commands. The port is `/dev/cu.usbmodem*` (native USB) or
`/dev/cu.wchusbserial*` (CH340). macOS 11+ and modern Linux need no extra driver.

## First-run configuration

No credentials are compiled into the firmware — everything is stored in NVS
(`Preferences`, namespace `paybox-cfg`) and entered through a web form.

1. On first boot the device starts an access point named **`357Paybox`**
2. Connect to it and open **http://192.168.5.1**
3. Fill in WiFi credentials, operating mode, and the backend URLs
4. The device reboots into normal operation

A firmware update via `pio run -t upload` does **not** erase NVS — settings
survive reflashing. If the device boots into AP-setup mode unexpectedly, NVS was
cleared and the configuration must be re-entered.

Required backend endpoints (set through the web form):

| Setting | Purpose |
|---|---|
| `pay_gen_qr` | returns a PromptPay QR payload for a given `amount` |
| `pay_chk_stat` | returns `{"success":true,"status":"succeeded"}` when paid |

## Screens

**Amount entry** — brand header with live WiFi indicator, amount in 48px with a
`THB` caption, always-visible keypad on the right (`00` key for fast round
amounts), outline *ยกเลิก* to clear and a teal *ยืนยัน* as the primary action.

**QR payment** — QR on a white elevated card (kept white deliberately: a dark
quiet zone breaks phone scanners), amount in teal, countdown pill, outline cancel.

**Result** — glowing badge in teal for success or red for timeout, auto-returns to
amount entry after 5 seconds.

Theme colours are nine `COL_*` defines at the top of `style_init()` in
`src/main.cpp` — change the whole look from one place.

## Layout

```
platformio.ini                 build config, board + partition settings
boards/esp32-s3-n16r8v.json    custom board definition (N16R8, qio_opi)
src/main.cpp                   all application + UI code
src/esp_bsp.*                  board support: display, touch, backlight
src/esp_lcd_axs15231b.*        AXS15231B QSPI display driver
src/esp_lcd_touch.*            capacitive touch driver
src/lv_port.*                  LVGL port layer (buffers, tick, flush)
src/sarabun_20.c, _28.c        Thai fonts (LVGL format)
src/web_server_html.h          configuration web form
include/lv_conf.h              LVGL configuration (the one actually used)
lib/qrcodegen                  QR generation
lib/PNGdec_bitbank2            PNG decoder
```

## Known issues

- **Flash usage is 75.4%** of the 2 MB app partition (`no_ota.csv`). Roughly
  515 KB of headroom. All Montserrat sizes are enabled in `lv_conf.h`; trimming
  the unused ones is the easiest win if space runs short.
- **Screen objects leak.** `create_qr_payment_screen()` calls `lv_obj_create(NULL)`
  and `lv_scr_load()` without deleting the outgoing screen, so one screen object
  is leaked per transaction cycle. Small individually, unbounded over weeks of
  uptime.
- **This revision has not been run on hardware yet** — it compiles clean, but the
  layout coordinates are hand-computed for 480×320 and want a visual check on a
  real panel, particularly a 9-digit amount at 48px in the 228px-wide clip region.

## Credit

Display and touch drivers are derived from the manufacturer's `DEMO_LVGL` example
for the JC3248W535EN, which is in turn based on Espressif's BSP code (Apache-2.0).
