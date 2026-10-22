# Kindle Click BLE 协议要点

依据 [zuyan9/Open-Kindle-Click-nRF52840](https://github.com/zuyan9/Open-Kindle-Click-nRF52840) 的 PROTOCOL.md，
本固件按此实现，已在 Kindle Paperwhite（第 12 代，固件 5.20.1）上跑通。

## 广播

- 广播包（ADV_IND）：Flags `0x06`（必须带「不支持经典蓝牙」位，否则 Kindle 会用经典蓝牙连接）
  + 厂商数据 `71 01 04 43`（必须是第一个 0xFF 字段）+ HID 服务 UUID；不能带 `0xFE03`。
- 设备名放在扫描响应里。
- 广播间隔 20 ms。

## GATT

- 设备信息服务：PnP ID `01 71 01 43 04 00 01`（VID 0x0171 / PID 0x0443）；固件版本会显示在 Kindle「设定 › 配件」。
- HID：只有一个 Application Collection，Report ID 1，每次 10 字节：
  `[modifier][保留][按键1..6][consumer 低][consumer 高]`。
  键盘键和亮度键放在同一个 collection 里，按住亮度键才会连续调节。
- Kindle 只接受 5 个键：

  | 功能 | 键码 |
  |---|---|
  | 下一页 | Keyboard `0x4E` |
  | 上一页 | Keyboard `0x4B` |
  | 亮度 + | Consumer `0x6F` |
  | 亮度 − | Consumer `0x70` |
  | 电源（让 Kindle 休眠） | Consumer `0x30` |

## 配对

- Just Works（无输入无输出）+ 绑定 + Secure Connections。
- 不下发本机的 IRK，但要接收 Kindle 的 IRK。
- 翻页器主动发起的安全请求会被 Kindle 忽略。

## 何时可以发键

Kindle 每次连接都会重新读取 GATT 并重建输入设备，在这之前发出的按键会被直接丢掉。所以本固件要等到：

1. 链路已加密，并且
2. Kindle 最后一次订阅通知或调整连接参数之后，800 ms 内没有新动作

才开始发键，最多等 5 秒。实测连接后约 2.5 s 就绪。

## 连接参数

Kindle 接受翻页器请求的 slave latency 30、监督超时 6 s；连接间隔固定为 20 ms。
