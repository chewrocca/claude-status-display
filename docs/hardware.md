# Hardware notes

Details about the board itself, the case, and how the firmware survives being left alone.
None of this is needed to use the thing; see the [README](../README.md) for that.

## The board

**Waveshare ESP32-C6-LCD-1.47**, non-touch. ST7789 172x320 panel, WS2812 LED, BOOT button
(GPIO 9) and RESET. Wi-Fi 6 (2.4 GHz only), BLE 5, and an 802.15.4 radio that goes unused,
all on a ceramic antenna. No touchscreen, IMU, buzzer, or battery circuit. A vibration motor
or piezo could go on a free GPIO via the header.

Build with `PartitionScheme=no_ota`; the radios need the full 2 MB app slot.

## The microSD slot

Optional, and **unverified on the unit in these photos.** The firmware ships SD support and
uses it for one thing: a history sample every five minutes appended to `/hist.csv` as
`epoch,week%,5h%,ctx%,state`, which seeds the weekly-burn curve at boot. Those samples have no
week anchor, so they are drawn without the even-spend diagonal and labeled a recent trend until
a payload arrives and the board can place them properly.

Running without a card is fine. The mount is retried every five minutes, so a card fitted later
starts working without a reset, and it mounts down a speed ladder from 20 MHz to 400 kHz.

From the board's netlist, not the wiki table: `SD_CS` is GPIO4 (TF pin 2), `SD_MISO` GPIO5
(pin 7), `SD_MOSI` GPIO6 (pin 3, shared with `LCD_DIN`), `SD_SCLK` GPIO7 (pin 5, shared with
`LCD_CLK`), and R15-R20 are 10K pull-ups to 3V3 on every SD line. Because MOSI and SCLK also
drive the panel, the card is clocked conservatively and every SD call is made from `loop()`, on
the same thread as the renderer, so the two never overlap a transaction.

There is no card-detect line wired to a GPIO, so the board cannot tell a card is inserted except
by talking to it. "No card" and "card that won't talk" are the same observation.

### Two corrections, in order

An earlier version of this project called the test card dead on the strength of a bit-banged
CMD0 probe. That was wrong. The probe was never checked against a card known to be good, and it
produced the same failing trace for a card that then mounted fine elsewhere.

What replaced it is narrower. Using ESP-IDF's own SD driver, with the display never initialized
and the radios down, this card fails at **CMD8 (`send_if_cond`)** — the card layer, before any
filesystem is involved, so formatting is not the explanation either:

```text
sdspi_host_init_device:  ESP_OK
sdmmc_init_sd_if_cond:   send_if_cond (1) returned 0x108
sdmmc_card_init:         ESP_ERR_INVALID_RESPONSE
```

That establishes only that **this card does not initialize in this board's slot.** Whether the
fault is the card, the socket, or the contacts is not something the board can distinguish, and
it has not been settled with a known-good reader. If you fit a card to your own board, expect it
to work, and let me know if it does.

## The case

The one in the photos is the
[ESP32-C6 with LCD Screen Enclosure Case](https://makerworld.com/en/models/2121443-esp32-c6-with-lcd-screen-enclosure-case#profileId-2296385)
on MakerWorld. It fits and looks good, but seating the board took more force than felt safe. Go
slowly, start with one corner, and expect the USB-C end to be the stubborn one. The bezel
overlaps the panel by a few pixels, which is why the firmware keeps text clear of the right edge.

Print it in a light or translucent filament so the LED under the board can light the case from
inside. Make sure the BOOT button stays reachable, since it is the only control.

Untested alternatives: a [snap-on lid enclosure](https://www.printables.com/model/1365867-esp32-c6-147inch-display-enclosure)
and a [reference CAD model of the board](https://www.printables.com/model/1633740-esp32-c6-lcd-147-reference-cad-model)
if you want to design your own. Cases for the *Touch* variant do not fit; the cutouts differ.

## Running unattended

- A 30 s task watchdog reboots the board if the main loop stalls. The framebuffer dump feeds it
  per chunk and aborts on a short write, so a client that walks out of Wi-Fi range mid-screenshot
  cannot wedge the device.
- No dynamic strings on the hot paths. Serial lines land in a fixed buffer, and a line that does
  not open with `{` is treated as one joined partway through and skipped to the next newline.
- One framebuffer is allocated once and shared across rotations. A failed allocation restarts
  cleanly instead of drawing into null.
- Bluetooth writes arrive on the BLE stack's task and are queued for the main loop rather than
  touching shared state directly.
- `/cmd` allowlists only the harmless UI verbs. mDNS registers its service once, not on every
  reconnect. The status poll is skipped when free heap is under 60 KB.
- `GET /info` reports `heap`, `minheap`, and `uptime_s` for long-term monitoring.

## Known limits

- After you approve a permission prompt there is no "approved" hook event, so the band stays on
  NEEDS YOU until that tool finishes and `PostToolUse` fires.
- Only one process can hold the serial port. Do not run the daemon by hand while the launchd
  agent is loaded; two writers interleave bytes and the device shows NO LINK.
- Host slots are capped at three machines. A fourth evicts the stalest, with no warning.
