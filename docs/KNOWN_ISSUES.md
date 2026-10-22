# Known issues

Evidence tags: `[Tested]` verified on real hardware by this project | `[Community]` third-party
sources, not independently verified | `[Inferred]` reasoned from the evidence, not tested |
`[Untested]` not yet verified.

## Kindle firmware behavior (can't be fixed on the remote's side)

### 1. Page turns don't reset the Kindle's auto-sleep timer (fixed in 5.20.1.0.2)

- **Fixed** `[Tested 2026-10-25, Kindle Paperwhite 12th gen, firmware 5.20.1.0.2]`: after updating the
  Kindle to 5.20.1.0.2, page turns from this remote reset the auto-sleep timer. Turning pages without
  touching the screen no longer lets the Kindle fall asleep. Nothing changed in this firmware, so the
  fix is on the Kindle's side. There are no official release notes for this version; it appeared a few
  days before the official Kindle Click went on sale (2026-10-28), so it was likely a fix made for the
  official remote `[Inferred]`. Whether the brightness keys also reset the timer on 5.20.1.0.2 is
  `[Untested]`.
- The rest of this section describes 5.20.1 and earlier and is kept for reference.
- **Symptom** `[Tested 2026-10-22, Kindle Paperwhite 12th gen, firmware 5.20.1]`: after the last
  touch on the screen, even if you keep turning pages with the remote, the Kindle turns off the
  frontlight once the sleep timer (Settings › Device Options) runs out, then goes to sleep and shows
  the lock screen.
- **The brightness keys behave the same** `[Tested]`: after one touch on the screen, repeatedly
  sending brightness +/− still lets the Kindle fall asleep on time.
- **Cause** `[Community]`:
  - The Kindle's `input_manager`, which handles remote keys, calls the power management service
    powerd in only three places: the power key calls `setPowerButtonIfActiveAsync(1)`, and
    brightness +/− call `stepBrightnessWithUI(±1)`. **Nothing** calls `setTouchSsTimeoutAsync`, which
    restarts the sleep countdown (source: the binary analysis in nes-dpad FINDINGS.md, 2026-10-08;
    zuyan9 PROTOCOL.md section 5 reaches the same conclusion).
  - Page keys are forwarded to a virtual input device named `kindle-input-pt`, which powerd doesn't
    treat as user activity.
  - powerd does have an interface for restarting the timer: the "keep awake" feature of the jailbreak
    tool kindle-button-mapper-rs runs
    `lipc-set-prop -i com.lab126.powerd touchScreenSaverTimeout 1` every minute
    (`src/main.rs:32-33`), and a jailbreak guide for the Paperwhite 12th gen (firmware 5.18.5) resets
    `screenSaverTimeout` on every page turn. Both need your own code running on the Kindle, which
    isn't possible without a jailbreak.
  - The 5.18.5 jailbreak guide already notes that "remote key presses don't count as user activity",
    so this has been the Kindle's behavior for a long time, not something new in 5.20.
- **Not specific to one remote implementation**: this project implements the public protocol item by
  item and sees the same behavior. Every known non-jailbreak alternative (zuyan9's nRF52840 and
  Android versions, nes-dpad) lists it as a known limitation.
- **On the theory that "the Kindle recognizes the official remote and leaves this problem in for
  third parties on purpose"**: we found no evidence for it. The Kindle identifies the remote only by
  VID/PID (in the advertisement and in the PnP ID), and pairing is plain Just Works with no
  cryptographic authentication. The problem is that the call is missing altogether, not a branch that
  treats devices differently, so judging from the code, the official remote would have had the same
  problem on 5.20.1 `[Inferred]`. After 5.20.1.0.2, this remote resets the timer with no changes on
  its side, which is consistent with the problem being in the Kindle firmware rather than the remote.
- **What to do**: update the Kindle to 5.20.1.0.2 or later. If you can't, set a longer sleep timeout
  on the Kindle (the zuyan9 README suggests the same).

### 2. The remote can put the Kindle to sleep but can't wake it

- `[Tested]` The sleep key works. Once the Kindle is asleep, pressing keys (even while Bluetooth is
  still connected) does nothing.
- `[Community]` The sleep key calls `setPowerButtonIfActiveAsync(1)`, which only works while the
  Kindle is in use. The wake-up interface `setWakeUpAsync` is never called. The Kindle accepts only
  5 keys and drops the wake key `KEY_WAKEUP`. About 100 seconds after going to sleep, the Kindle turns
  Bluetooth off. The official Kindle Click instructions also only mention "hold both side buttons to
  put the Kindle to sleep".
- In practice: press the Kindle's power button to wake it. The remote reconnects within about 2–4
  seconds and you can turn pages again. Keys pressed before the Kindle is ready are dropped, not
  queued.

### 3. Holding a page key turns only one page

`[Community]` By design on the Kindle (its input driver drops key repeat events). Holding a
brightness key keeps adjusting.

### 4. The Kindle caches the remote's GATT layout

If things misbehave after you change the GATT services or the HID report descriptor: "forget" the
device on the Kindle, clear the remote's pairing with the serial command `c`, then pair again.

## This firmware and its tools

1. **No idle auto power-off**: unless you turn it off, the remote keeps advertising at a 20 ms
   interval and draining the battery.
2. **Can't flash while the control script is running**: the script holds the serial port, so esptool
   can't connect. `tools/flash.sh` checks for this first and refuses if the port is busy.
3. **Opening the S3's USB serial port on macOS can reset the chip** `[Tested]`: pyserial sets DTR
   before RTS, briefly producing DTR=0 / RTS=1, which is exactly the reset signal.
   `tools/kindle_remote.py` now opens the port with both held high, then lowers RTS first and DTR
   second.
4. **Running without a terminal (tty)**: `flash.sh` needs `echo yes |` instead of a manual
   confirmation; use the control script in non-interactive mode.
5. **The tsuuraifluens/kindle-curie docs say "single-key sleep/wake" passed real-device testing**
   `[Community]`: its firmware only sends Keyboard `0x66` (`KEY_POWER`) and has no way to wake the
   Kindle, and the reverse-engineering notes in the same repository
   (`docs/reverse_engineering/input_mapping.md`) conclude that "`0x66` can't wake a Kindle showing the
   screensaver". We believe that test only verified sleep.
