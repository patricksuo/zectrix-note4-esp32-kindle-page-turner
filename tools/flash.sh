#!/usr/bin/env bash
# Write only the app to ota_0 @ 0x20000 (2 MB), then read it back to verify.
#
# ⚠️ Precondition: the device already has the partition layout from firmware/partitions.csv.
#    In the factory layout 0x20000 is not ota_0; **do not** use this script on a device in factory state.
# ⚠️ Not `idf.py flash`: it also writes the bootloader (0x0) and partition table (0x8000).
# ⚠️ No --baud 921600: the S3's native USB-JTAG default rate is faster and more stable.
#
# Usage:
#   tools/flash.sh firmware/build/kindle_page_turner.bin
#   PORT=/dev/cu.usbmodemXXX tools/flash.sh ...
#   echo yes | tools/flash.sh ...      # non-interactive (no terminal)
set -euo pipefail

readonly OTA0_OFFSET=0x20000
readonly OTA0_MAX_SIZE=$((0x200000))

PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"
[[ -n "$PORT" ]] || { echo "No serial device found. Plug in USB, or set PORT=..." >&2; exit 1; }
if lsof "$PORT" >/dev/null 2>&1; then
    echo "❌ $PORT is busy (control script still running? quit it first):" >&2
    lsof "$PORT" | tail -n +2 >&2
    exit 1
fi
esp() { esptool --port "$PORT" "$@"; }

BIN="${1:-}"
[[ -f "$BIN" ]] || { echo "Usage: $0 <app.bin>" >&2; exit 1; }

SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
if (( SIZE > OTA0_MAX_SIZE )); then
    echo "❌ image $SIZE B > ota_0 limit ${OTA0_MAX_SIZE}, would overflow into ota_1" >&2
    exit 1
fi

python3 - "$BIN" <<'PY' || exit 1
import struct, sys
d = open(sys.argv[1], 'rb').read(0x120)
if d[0] != 0xE9:
    sys.exit(f"❌ magic=0x{d[0]:02X}, not an ESP-IDF app image")
if struct.unpack('<H', d[12:14])[0] != 9:
    sys.exit("❌ not an ESP32-S3 image")
if struct.unpack('<I', d[0x20:0x24])[0] != 0xABCD5432:
    sys.exit("❌ missing esp_app_desc, not a complete app image")
print("✅ image check passed: ESP32-S3 app")
PY

# if otadata points at ota_1, the freshly written ota_0 won't be booted
boot_slot() {
    local tmp; tmp="$(mktemp)"
    if ! esp read-flash 0xd000 0x2000 "$tmp" >/dev/null 2>&1; then
        rm -f "$tmp"; echo "unknown"; return
    fi
    python3 - "$tmp" <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
best, slot = -1, None
for base in (0, 0x1000):
    e = d[base:base+32]
    seq, = struct.unpack('<I', e[0:4])
    state, crc = struct.unpack('<II', e[24:32])
    if seq == 0xFFFFFFFF or crc != (zlib.crc32(e[0:4], 0xFFFFFFFF) & 0xFFFFFFFF) or state in (3, 4):
        continue
    if seq > best:
        best, slot = seq, (seq - 1) % 2
print(f"ota_{slot}" if slot is not None else "ota_0")
PY
    rm -f "$tmp"
}

echo "Target: ota_0 @ $OTA0_OFFSET ($(( SIZE / 1024 )) KB)"
SLOT="$(boot_slot)"
echo "otadata: will boot from ${SLOT}"
[[ "$SLOT" == "unknown" ]] && echo "⚠️  otadata read failed; can't confirm the boot slot" >&2
echo "⚠️  Writes 0x20000 only; doesn't touch bootloader / partition table / nvs / ota_1 / content."
read -rp "Start flashing? (yes/NO) " ans
[[ "$ans" == "yes" ]] || { echo "Cancelled"; exit 1; }

esp write-flash "$OTA0_OFFSET" "$BIN"

TMP="$(mktemp)"
esp read-flash "$OTA0_OFFSET" "$SIZE" "$TMP" >/dev/null
if cmp -s "$BIN" "$TMP"; then echo "✅ read-back matches"; else echo "❌ read-back mismatch! Reflash before powering on" >&2; rm -f "$TMP"; exit 1; fi
rm -f "$TMP"
echo "✅ Flash complete."
[[ "$SLOT" == "ota_0" || "$SLOT" == "unknown" ]] || \
    echo "⚠️  otadata points at ${SLOT}: ota_0 won't be booted. Erase otadata (0xd000, 0x2000) first, then reboot." >&2
