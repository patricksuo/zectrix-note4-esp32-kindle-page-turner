# NOTE4 Kindle Page Turner

把 **ZECTRIX NOTE4** 变成 Kindle 蓝牙翻页器。

Kindle 会把它识别为官方翻页器 **Kindle Click**，**不需要越狱**，在 Kindle 的蓝牙设置里配对就能用。

## 功能

- 机身按键翻页、调节前光亮度、让 Kindle 休眠
- 向 Kindle 报告翻页器电量
- 墨水屏显示连接状态和电量（日文界面）
- 断开后自动重连，配对记录重启后保留
- 可选：用 USB 连接 Mac，用电脑键盘翻页

## 兼容性

- **翻页器**：ZECTRIX NOTE4（ESP32-S3 N16R8，4.2" 400×300 墨水屏）
- **Kindle**：已在 Kindle Paperwhite（第 12 代）固件 5.20.1 上测试。其他支持 Kindle Click 的型号和固件版本未测试，欢迎反馈。

## 已知限制

这些是 Kindle 固件本身的行为，所有以 Kindle Click 身份连接的翻页器都一样：

- **用翻页器翻页不会重置 Kindle 的自动休眠计时。** 只用翻页器翻页、不碰屏幕的话，Kindle
  会按设定的时间照常休眠。建议在「设定 › 设备选项」里把休眠时间调长。
- **翻页器不能唤醒 Kindle。** 先按 Kindle 的电源键，约 2–4 秒后翻页器会自动重连。
- **按住翻页键只翻一页**（按住亮度键可以连续调节）。

另外，本固件没有闲置自动关机，不用时请手动关机。

原因分析见 [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)。

## 安装

需要 [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/)（自带 `esptool` 和 `espefuse`）和一根 USB-C 数据线。
下面的 `PORT` 在 macOS 上一般是 `/dev/cu.usbmodem*`。

### 构建

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
```

### 首次刷机

> [!WARNING]
> 这个流程还没有在出厂状态的 NOTE4 上完整验证过。**一定要先做第 2 步的整片备份**，
> 出问题时可以用它恢复到出厂状态。

```bash
# 1. 确认没有开启 Secure Boot / Flash 加密（只读，不会修改设备）
#    需要看到 SECURE_BOOT_EN = False、SPI_BOOT_CRYPT_CNT = 0；否则不要继续
espefuse --port PORT summary

# 2. 备份整片 16 MB flash，妥善保存
esptool --port PORT read-flash 0 0x1000000 note4-backup.bin

# 3. 擦除 nvs 区域（出厂时这里的数据和本固件不兼容，会导致配对记录无法保存）
esptool --port PORT erase-region 0x10000 0x6000

# 4. 写入 bootloader、分区表和固件
cd firmware && idf.py -p PORT flash
```

恢复出厂：`esptool --port PORT write-flash 0 note4-backup.bin`

> [!CAUTION]
> 不要执行任何 `espefuse burn*` 命令，eFuse 一旦写入就无法撤销。

### 更新固件

首次刷机之后，用 `tools/flash.sh` 更新固件。它只写固件分区并回读校验，不会动 bootloader、分区表和配对记录：

```bash
tools/flash.sh firmware/build/kindle_page_turner.bin
```

## 配对

1. 打开翻页器，LED 每秒短闪一次，表示正在等待连接。
2. 在 Kindle 上打开「设定 › Wi-Fi 和蓝牙 › 蓝牙设备」，扫描后选择「Kindle Click」。
3. 不需要输入配对码。LED 常亮，就可以翻页了。

之后每次开机都会自动重连。

## 使用

| 操作 | 按法 |
|---|---|
| 下一页 / 上一页 | 下翻键 / 上翻键 |
| 亮度 + / − | 按住确认键，再按上翻键 / 下翻键 |
| 让 Kindle 休眠 | 同时按住上翻键和下翻键 1 秒 |
| 翻页器关机 | 按住确认键 5 秒 |
| 翻页器开机 | 按确认键 |

| LED | 状态 |
|---|---|
| 每秒短闪一次 | 等待连接 |
| 快闪 | 已连接，正在准备 |
| 常亮 | 可以翻页（发送按键时会短暂熄灭） |

**用电脑键盘翻页**：USB 连接 Mac 后运行 `uv run tools/kindle_remote.py`，空格或 → 下一页，← 上一页。
详见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 开发

- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)：串口命令、代码结构、引脚定义，以及修改固件前必须了解的硬件注意事项
- [docs/PROTOCOL.md](docs/PROTOCOL.md)：Kindle Click 的 BLE 协议要点

## 致谢

Kindle Click 协议来自以下项目的逆向分析：

- [zuyan9/Open-Kindle-Click-nRF52840](https://github.com/zuyan9/Open-Kindle-Click-nRF52840)：协议文档（PROTOCOL.md）
- [zuyan9/Open-Kindle-Click-Android](https://github.com/zuyan9/Open-Kindle-Click-Android)
- [bydash-xyz/nes-dpad-kindle-remote](https://github.com/bydash-xyz/nes-dpad-kindle-remote)：ESP32-S3 实现与 Kindle 固件分析（FINDINGS.md）

相关项目：

- [tsuuraifluens/kindle-curie](https://github.com/tsuuraifluens/kindle-curie)：另一套 ESP32 实现
- [zampierilucas/kindle-button-mapper-rs](https://github.com/zampierilucas/kindle-button-mapper-rs)、
  [JPHutchins/ble-page-turner-investigations](https://github.com/JPHutchins/ble-page-turner-investigations)：越狱 Kindle 上的方案

## License

代码采用 [MIT License](LICENSE)。

`firmware/main/shared/font_jp*.c` 中的字形由 Noto Sans CJK JP、Noto Sans Math（SIL Open Font License 1.1）
和東雲フォント（public domain）生成，遵循各自的许可证。

Kindle 是 Amazon.com, Inc. 的商标。本项目与 Amazon 及 ZECTRIX 无关。
