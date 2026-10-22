# Development notes

## Read this before changing the firmware: hardware pitfalls

The NOTE4's main power is latched by GPIO17. Getting it wrong can cut power to the whole device or
even damage the hardware.

1. **At boot, first drive GPIO17 high to latch the power, then drive the amplifier's GPIO46 low and
   keep it there** (see `safe_early_init` in `main.c`).
2. **Before entering deep sleep, GPIO17 must be held high with an RTC hold.** A plain `gpio_hold_en`
   doesn't hold, and the device loses power as soon as it's released. The exact steps are decided by
   `shared/safe_state_sleep_model.c`; `kr_power.c` only calls it. If any step can't be confirmed, the
   firmware restarts instead of sleeping. If it can't even confirm that GPIO17 is high, it stops in
   place and neither restarts nor sleeps.
3. For routine updates use only `tools/flash.sh`, never `idf.py flash` (which rewrites the bootloader
   and partition table). The firmware never erases nvs and never writes the content partition.
4. Never run any `espefuse burn*` command.

## Pinout

| Function | GPIO |
|---|---|
| OK / Up / Down buttons | 0 / 39 / 18 |
| Main power latch | 17 |
| Amplifier enable | 46 |
| Audio power AVDD | 42 |
| Display power | 6 |
| LED (active low) | 3 |
| Battery voltage ADC (1:2 divider) | 4 |
| Charging (active low) / fully charged | 2 / 1 |

## Partition table

See `firmware/partitions.csv`: nvs at 0x10000, ota_0 at 0x20000 (2 MB). At runtime the firmware reads
the partition table at 0x8000 on the device. `tools/flash.sh` writes only ota_0, so the device must
already have this partition table (see the first-time install in [INSTALL.md](INSTALL.md)).

`flash.sh` checks otadata: if it points to ota_1, the freshly written ota_0 won't boot, and you need to
erase otadata first (0xd000, 0x2000).

## Mac control script

```bash
uv run tools/kindle_remote.py          # interactive: Space/→ next page, ← previous, =/- brightness, s sleep, ? status, q quit
uv run tools/kindle_remote.py fb 3     # non-interactive: send f, then b, then show 3 seconds of logs
uv run tools/kindle_remote.py - 60     # send nothing, just show 60 seconds of logs
```

- Interactive mode must run in a terminal.
- The script holds the serial port while it runs, so `flash.sh` refuses to flash. Quit the script first.
- On macOS, opening the ESP32-S3's USB serial port can reset the chip (the order in which pyserial
  sets DTR/RTS briefly produces a reset signal). The script already handles this.
- Without a terminal (e.g. in CI), use `echo yes | tools/flash.sh ...` instead of a manual confirmation.

### Serial commands

| Command | Action |
|---|---|
| `f` / `b` | Next / previous page |
| `u` / `d` | Brightness up / down one step |
| `F` `U` `D` | Hold the corresponding key for 2 seconds |
| `s` | Put the Kindle to sleep |
| `0`–`9` / `B` | Set the battery level reported to the Kindle by hand (for testing the low-battery warning) |
| `R` | Go back to reporting the real battery level |
| `P` | Power off |
| `c` | Clear the pairing |
| `?` / `h` | Status / help |

## Re-pairing

The Kindle caches the remote's GATT layout. After changing the GATT services or the HID report
descriptor, "forget" the device on the Kindle, clear the remote's pairing with the serial command `c`,
then pair again.

## Code layout

| File | Purpose |
|---|---|
| `firmware/main/main.c` | Boot-time safety setup, buttons, serial commands, turning button presses into HID reports (presses are dropped while the Kindle isn't ready) |
| `firmware/main/kr_ble.c` | Bluetooth: advertising, GATT services (HID / battery / device info), pairing, readiness detection, battery notifications, connection parameters |
| `firmware/main/kr_ui.c` | E-ink screen (its own task; redraws only on state changes, immediately when the connection becomes ready, otherwise after the state has been stable for 2 seconds) |
| `firmware/main/kr_batt.c` | Reads battery voltage and charging state and passes them to `shared/battery_model` to compute the level |
| `firmware/main/kr_power.c` | Power-off: disconnect Bluetooth → show the power-off screen → turn off each power rail → switch GPIO17 to RTC hold → deep sleep |
| `firmware/main/epd_*.c`, `board_pins.h` | Display driver (SPI) and pin definitions |
| `firmware/main/shared/` | Chip-independent code: display protocol, framebuffer, fonts, battery calculation, GPIO17 deep-sleep handoff steps |
| `tools/flash.sh` | Writes only ota_0 and reads it back to verify |
| `tools/kindle_remote.py` | Control and logging from a Mac |

### Button timing

After Up or Down is pressed, the firmware waits 50 ms to see whether it's part of a combination, so
page turns have about 50 ms of latency.
