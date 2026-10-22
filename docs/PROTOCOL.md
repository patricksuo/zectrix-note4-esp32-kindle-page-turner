# Build your own Kindle Click remote

Any Bluetooth LE device that follows the rules below is accepted by the Kindle as Amazon's
Kindle Click remote, with no jailbreak. This page covers everything you need to build one on
other hardware: what to advertise, the GATT table byte by byte, pairing, and when the Kindle is
ready to receive keys.

It's based on the PROTOCOL.md in [zuyan9/Open-Kindle-Click-nRF52840](https://github.com/zuyan9/Open-Kindle-Click-nRF52840)
and verified with this firmware on a Kindle Paperwhite (12th gen, firmware 5.20.1 and 5.20.1.0.2).
The reference implementation is [`firmware/main/kr_ble.c`](../firmware/main/kr_ble.c) (ESP-IDF + NimBLE).

## At a glance

1. **Advertise** with Amazon's company ID and the Kindle Click product ID.
2. **Expose** an HID service, a Battery service and a Device Information service.
3. **Pair** with Just Works and bonding.
4. **Wait** until the Kindle has finished setting up the remote.
5. **Send** 10-byte input reports. The Kindle accepts five keys.

## 1. Advertising

| Field | Value | Notes |
|---|---|---|
| Flags | `0x06` | Must include "BR/EDR not supported", or the Kindle connects over Classic Bluetooth |
| Manufacturer data (`0xFF`) | `71 01 04 43` | Company ID 0x0171 (little-endian) + PID 0x0443 (big-endian). Must be the first `0xFF` field |
| Complete 16-bit UUIDs | `0x1812` | HID service |
| Scan response: name | anything | The Kindle lists the device as "Kindle Click" regardless |

- **Don't** include the `0xFE03` service UUID; it makes the Kindle skip its Amazon remote handling.
- Advertise connectable and undirected. We use a 20 ms interval: the Kindle reconnects through a
  background scan, so a shorter interval reconnects faster.
- Use a fixed address (public or static random), not a resolvable private address.

## 2. GATT table

Put the HID service first, so later changes to the other services don't move its handles.

| Service | Characteristic | Properties | Value |
|---|---|---|---|
| HID `0x1812` | HID Information `0x2A4A` | Read | `11 01 00 03` (bcdHID 1.11, country 0, RemoteWake + NormallyConnectable) |
| | Report Map `0x2A4B` | Read | See below |
| | HID Control Point `0x2A4C` | Write without response | Accept and ignore |
| | Report `0x2A4D` | Read (encrypted), Notify | The 10-byte input report; Report Reference descriptor `0x2908` = `01 01` (ID 1, Input) |
| Battery `0x180F` | Battery Level `0x2A19` | Read, Notify | 0–100 |
| Device Information `0x180A` | PnP ID `0x2A50` | Read | `01 71 01 43 04 00 01` (Bluetooth SIG source, VID 0x0171, PID 0x0443, version 0x0100) |
| | Firmware Revision `0x2A26` | Read | Any string; shown under Settings › Accessories on the Kindle |
| | Manufacturer Name `0x2A29` | Read | Any string |

**Report map.** Keyboard and consumer keys must live in a single Application Collection. The Kindle
then creates one input device with key repeat enabled, which is what makes a held brightness key
keep adjusting.

```c
0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,             // Generic Desktop / Keyboard / Application
0x85, 0x01,                                     //   Report ID 1
0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,             //   Modifiers: 8 × 1 bit
0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
0x75, 0x08, 0x95, 0x01, 0x81, 0x01,             //   Reserved byte
0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
0x75, 0x08, 0x95, 0x06, 0x81, 0x00,             //   6 key slots
0x05, 0x0C,                                     //   Consumer page
0x19, 0x00, 0x2A, 0xFF, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x03,
0x75, 0x10, 0x95, 0x01, 0x81, 0x00,             //   One 16-bit consumer usage
0xC0,
```

## 3. Sending keys

Each input report is 10 bytes:

```
[modifier][reserved][key1][key2][key3][key4][key5][key6][consumer low][consumer high]
```

The Kindle accepts only these five keys; anything else is dropped.

| Action | Where it goes | Code |
|---|---|---|
| Next page | Key slot | Keyboard `0x4E` (Page Down) |
| Previous page | Key slot | Keyboard `0x4B` (Page Up) |
| Brightness + | Consumer bytes | `0x006F` |
| Brightness − | Consumer bytes | `0x0070` |
| Sleep | Consumer bytes | `0x0030` (Power) |

- Send a report with the key down, then an all-zero report to release it. Every report carries the
  full key state, so if a notification fails you can simply resend the latest one.
- We hold each press for at least 30 ms before sending the release.
- Holding a page key turns only one page (the Kindle drops key repeats for them). Holding a
  brightness key keeps adjusting.
- Before sending Sleep, release any brightness key, or the Kindle keeps changing the brightness.

## 4. Pairing

- **Just Works**: IO capability NoInputNoOutput, bonding on, Secure Connections on, no MITM.
  If you declare a keyboard or display capability, the Kindle asks for a PIN.
- **Key distribution**: don't distribute your own IRK (keep advertising from a fixed address), but
  do request the Kindle's IRK. The Kindle reconnects from a resolvable private address, and without
  its IRK you can't match it to the bond.
- **Let the Kindle start security.** It ignores security requests initiated by the remote.
- **Re-pairing**: if the Kindle forgot the remote and pairs again, drop the old bond and accept the
  new one.

## 5. Waiting until the Kindle is ready

On every connection, including reconnections, the Kindle re-reads the GATT table and rebuilds its
input device. Keys sent before that finishes are silently dropped. Our firmware treats the link as
ready when it's encrypted and either:

- **800 ms have passed with nothing new** since the Kindle last enabled a notification (a CCCD
  write) or finished discovery. Discovery runs at a 7.5 ms connection interval; leaving 7.5 ms, or
  settling at 20 ms with no latency, means it's done. Or:
- **5 seconds have passed** since encryption (fallback).

In practice the link is ready about 2.5 s after connecting. Drop key presses that come in before
that rather than queueing them, so the reader doesn't get a burst of page turns.

## 6. Battery level

The Kindle only picks up the battery level from notifications, and only once the HID device is set
up. We notify once the link is ready, again 1 s after any new CCCD write, and every 15 minutes.

## 7. Connection parameters

The Kindle keeps the connection interval at 20 ms but accepts the latency and timeout the remote
asks for. 1.5 s after the link is ready, we request interval 20 ms, slave latency 30 and supervision
timeout 6 s to save power.

## Testing tips

- The Kindle caches the GATT layout. After changing any service or the report map, "forget" the
  remote on the Kindle and clear the bonds on your device before pairing again.
- A BLE scanner app such as nRF Connect is handy for checking your advertising data against the
  table above.
- Some limits are on the Kindle's side and can't be fixed by the remote: it can't wake the Kindle,
  and on firmware 5.20.1 and earlier page turns don't reset the auto-sleep timer. See
  [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Porting this firmware to another ESP32 board

`kr_ble.c` depends only on ESP-IDF and NimBLE, so it carries over to other ESP32 boards as is.
The NOTE4-specific parts are the buttons and pins (`main.c`, `board_pins.h`), the e-paper screen
(`epd_*.c`, `kr_ui.c`), battery reading (`kr_batt.c`) and the GPIO17 power latch (`kr_power.c`,
`safe_state.h`). See [DEVELOPMENT.md](DEVELOPMENT.md) for the code layout.
