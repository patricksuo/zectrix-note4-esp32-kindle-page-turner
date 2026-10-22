#!/usr/bin/env bash
# 只把 app 写进 ota_0 @ 0x20000（2 MB），然后回读校验。
#
# ⚠️ 前提：设备上已经是 firmware/partitions.csv 的分区布局。
#    出厂布局下 0x20000 不是 ota_0，**不要**对出厂状态的设备用本脚本。
# ⚠️ 不用 `idf.py flash`：它会连带写 bootloader(0x0) 和分区表(0x8000)。
# ⚠️ 不加 --baud 921600：S3 原生 USB-JTAG 默认速率更快更稳。
#
# 用法:
#   tools/flash.sh firmware/build/kindle_page_turner.bin
#   PORT=/dev/cu.usbmodemXXX tools/flash.sh ...
#   echo yes | tools/flash.sh ...      # 非交互（没有终端时）
set -euo pipefail

readonly OTA0_OFFSET=0x20000
readonly OTA0_MAX_SIZE=$((0x200000))

PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"
[[ -n "$PORT" ]] || { echo "找不到串口设备。插上 USB，或用 PORT=... 指定。" >&2; exit 1; }
if lsof "$PORT" >/dev/null 2>&1; then
    echo "❌ $PORT 被占用（控制脚本还开着？先退出）：" >&2
    lsof "$PORT" | tail -n +2 >&2
    exit 1
fi
esp() { esptool --port "$PORT" "$@"; }

BIN="${1:-}"
[[ -f "$BIN" ]] || { echo "用法: $0 <app.bin>" >&2; exit 1; }

SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
if (( SIZE > OTA0_MAX_SIZE )); then
    echo "❌ 镜像 $SIZE B > ota_0 上限 ${OTA0_MAX_SIZE}，会溢出到 ota_1" >&2
    exit 1
fi

python3 - "$BIN" <<'PY' || exit 1
import struct, sys
d = open(sys.argv[1], 'rb').read(0x120)
if d[0] != 0xE9:
    sys.exit(f"❌ magic=0x{d[0]:02X}，不是 ESP-IDF app 镜像")
if struct.unpack('<H', d[12:14])[0] != 9:
    sys.exit("❌ 不是 ESP32-S3 镜像")
if struct.unpack('<I', d[0x20:0x24])[0] != 0xABCD5432:
    sys.exit("❌ 缺少 esp_app_desc，不是完整 app 镜像")
print("✅ 镜像校验通过：ESP32-S3 app")
PY

# otadata 指向 ota_1 时，写好的 ota_0 不会被引导
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

echo "目标: ota_0 @ $OTA0_OFFSET ($(( SIZE / 1024 )) KB)"
SLOT="$(boot_slot)"
echo "otadata：将从 ${SLOT} 启动"
[[ "$SLOT" == "unknown" ]] && echo "⚠️  otadata 读取失败——无法确认启动槽" >&2
echo "⚠️  只写 0x20000；不碰 bootloader / 分区表 / nvs / ota_1 / content。"
read -rp "开始烧录？(yes/NO) " ans
[[ "$ans" == "yes" ]] || { echo "已取消"; exit 1; }

esp write-flash "$OTA0_OFFSET" "$BIN"

TMP="$(mktemp)"
esp read-flash "$OTA0_OFFSET" "$SIZE" "$TMP" >/dev/null
if cmp -s "$BIN" "$TMP"; then echo "✅ 回读一致"; else echo "❌ 回读不一致！先重刷再上电" >&2; rm -f "$TMP"; exit 1; fi
rm -f "$TMP"
echo "✅ 烧录完成。"
[[ "$SLOT" == "ota_0" || "$SLOT" == "unknown" ]] || \
    echo "⚠️  otadata 指向 ${SLOT}：ota_0 不会被引导。需要先擦除 otadata（0xd000，0x2000）再重启。" >&2
