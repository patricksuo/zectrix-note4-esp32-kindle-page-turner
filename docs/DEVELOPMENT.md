# 开发说明

## 修改固件前必读：硬件注意事项

NOTE4 的主电源由 GPIO17 锁存，处理不当会整机断电甚至损坏硬件。

1. **开机第一步把 GPIO17 拉高锁住电源，第二步把功放 GPIO46 拉低并保持**（见 `main.c` 的 `safe_early_init`）。
2. **进入深度睡眠前，GPIO17 必须用 RTC hold 保持高电平。** 普通的 `gpio_hold_en` 保持不住，
   一松开就会整机断电。具体步骤由 `shared/safe_state_sleep_model.c` 决定，`kr_power.c` 只负责调用。
   任何一步无法确认就重启而不睡；如果连「GPIO17 为高」都无法确认，就原地停住，既不重启也不睡。
3. 日常更新只用 `tools/flash.sh`，不要用 `idf.py flash`（它会重写 bootloader 和分区表）。
   固件不会擦除 nvs，也不会写 content 分区。
4. 不要执行任何 `espefuse burn*` 命令。

## 引脚

| 功能 | GPIO |
|---|---|
| 确认键 / 上翻键 / 下翻键 | 0 / 39 / 18 |
| 主电源锁存 | 17 |
| 功放使能 | 46 |
| 音频电源 AVDD | 42 |
| 屏幕电源 | 6 |
| LED（低电平亮） | 3 |
| 电池电压 ADC（1:2 分压） | 4 |
| 充电中（低有效） / 充满 | 2 / 1 |

## 分区表

见 `firmware/partitions.csv`：nvs 在 0x10000，ota_0 在 0x20000（2 MB）。固件运行时读取设备上
0x8000 处的分区表；`tools/flash.sh` 只写 ota_0，所以设备上必须已经是这张分区表（首次刷机步骤见 README）。

`flash.sh` 会检查 otadata：如果它指向 ota_1，写好的 ota_0 不会被引导，需要先擦除 otadata（0xd000，0x2000）。

## Mac 控制脚本

```bash
uv run tools/kindle_remote.py          # 交互：空格/→ 下一页，← 上一页，=/- 亮度，s 休眠，? 状态，q 退出
uv run tools/kindle_remote.py fb 3     # 非交互：依次发送 f、b，然后显示 3 秒日志
uv run tools/kindle_remote.py - 60     # 不发命令，只显示 60 秒日志
```

- 交互模式需要在终端里运行。
- 脚本运行期间一直占用串口，这时 `flash.sh` 会拒绝烧录，先退出脚本。
- 在 macOS 上打开 ESP32-S3 的 USB 串口可能让芯片重启（pyserial 设置 DTR/RTS 的顺序会短暂产生复位信号）。
  脚本已经处理了这一点。
- 在没有终端的环境里（例如 CI），用 `echo yes | tools/flash.sh ...` 代替手动确认。

### 串口命令

| 命令 | 作用 |
|---|---|
| `f` / `b` | 下一页 / 上一页 |
| `u` / `d` | 亮度调一档 |
| `F` `U` `D` | 对应键按住 2 秒 |
| `s` | 让 Kindle 休眠 |
| `0`–`9` / `B` | 手动设定报告给 Kindle 的电量（测试低电量提示用） |
| `R` | 恢复报告真实电量 |
| `P` | 关机 |
| `c` | 清除配对记录 |
| `?` / `h` | 状态 / 帮助 |

## 重新配对

Kindle 会缓存翻页器的 GATT 结构。修改过 GATT 服务或 HID 报告描述符之后，先在 Kindle 上「忘记」该设备，
再用串口命令 `c` 清除翻页器的配对记录，然后重新配对。

## 代码结构

| 文件 | 作用 |
|---|---|
| `firmware/main/main.c` | 开机安全设置、按键、串口命令、把按键转成 HID 报告（Kindle 未就绪时按键直接丢弃） |
| `firmware/main/kr_ble.c` | 蓝牙：广播、GATT 服务（HID / 电池 / 设备信息）、配对、就绪判断、电量通知、连接参数 |
| `firmware/main/kr_ui.c` | 墨水屏画面（独立任务；状态变化才刷新，连接就绪立即刷新，其他变化稳定 2 秒后再刷新） |
| `firmware/main/kr_batt.c` | 读电池电压和充电状态，交给 `shared/battery_model` 计算电量 |
| `firmware/main/kr_power.c` | 关机：断开蓝牙 → 显示关机画面 → 关闭各路电源 → GPIO17 切换到 RTC hold → 深度睡眠 |
| `firmware/main/epd_*.c`、`board_pins.h` | 屏幕驱动（SPI）与引脚定义 |
| `firmware/main/shared/` | 与芯片无关的通用代码：屏幕协议、帧缓冲、字体、电量计算、GPIO17 深睡交接步骤 |
| `tools/flash.sh` | 只写 ota_0 并回读校验 |
| `tools/kindle_remote.py` | Mac 端控制与日志 |

### 按键时序

上下键按下后会等 50 ms，判断是不是组合键，所以翻页有约 50 ms 的延迟。
