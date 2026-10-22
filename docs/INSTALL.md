# Installation guide

## What you need

- A ZECTRIX NOTE4 and a USB-C data cable
- [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/), which includes `esptool` and `espefuse`

In the commands below, replace `PORT` with the NOTE4's serial port (usually `/dev/cu.usbmodem*`
on macOS). Run them from the root of this repository.

## First-time install

> [!WARNING]
> This process hasn't been fully verified on a NOTE4 in its factory state.
> **Don't skip the backup in step 3.** It's how you get the original software back.

```bash
# 1. Build the firmware
idf.py -C firmware set-target esp32s3
idf.py -C firmware build

# 2. Check that Secure Boot and flash encryption are off (read-only, changes nothing).
#    You should see SECURE_BOOT_EN = False and SPI_BOOT_CRYPT_CNT = 0. Otherwise, stop here.
espefuse --port PORT summary

# 3. Back up the whole 16 MB flash and keep the file somewhere safe
esptool --port PORT read-flash 0 0x1000000 note4-backup.bin

# 4. Erase the nvs region (the factory data there prevents pairings from being saved)
esptool --port PORT erase-region 0x10000 0x6000

# 5. Flash the bootloader, partition table and firmware
idf.py -C firmware -p PORT flash
```

> [!CAUTION]
> Never run any `espefuse burn*` command. eFuse changes are permanent.

## Pair with your Kindle

1. Press **OK** to turn the remote on.
2. On your Kindle, go to **Settings › Wi-Fi & Bluetooth › Bluetooth Devices** and pick **Kindle Click**.
   No PIN is needed.

The LED shows where things stand:

| LED | Meaning |
|---|---|
| Short blink every second | Waiting for the Kindle |
| Fast blinking | Connected, getting ready |
| Steady on | Ready to turn pages (flickers off briefly with each button press) |

After waking your Kindle, the remote reconnects within a few seconds.

## Update to a newer version

After the first install, use `tools/flash.sh`. It writes only the firmware partition and verifies
it, leaving the bootloader, partition table and saved pairing untouched:

```bash
tools/flash.sh firmware/build/kindle_page_turner.bin
```

## Restore the original software

```bash
esptool --port PORT write-flash 0 note4-backup.bin
```
