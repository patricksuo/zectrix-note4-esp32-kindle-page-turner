/* BLE 侧：伪装 Kindle Click（VID 0x0171 / PID 0x0443）的 HID-over-GATT 外设。
 * 协议出处：zuyan9 PROTOCOL.md（Kindle 5.20.1 逆向）+ nes-dpad protocol_test 实测，
 * 摘要在 ../README.md。所有函数都在 app 任务里调；GAP 事件跑在 NimBLE host 任务。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KR_REPORT_LEN 10 /* [modifier][reserved][key1..key6][consumer_lo][consumer_hi] */

void kr_ble_start(void);

bool kr_ble_connected(void);
/* 已加密，且 Kindle 最后一次 CCCD 写入 / 链路变更之后安静 800 ms（或加密后 5 s 兜底）。
 * 在此之前发的报告会被 Kindle 静默丢弃。 */
bool kr_ble_ready(void);
bool kr_ble_has_bonds(void);

/* 只在已订阅时发；false = 没发出去（调用方下一轮重试） */
bool kr_ble_send_report(const uint8_t r[KR_REPORT_LEN]);

void kr_ble_set_battery(uint8_t pct);
/* 每轮 loop 调：电量通知（连上 / 订阅后 / 每 15 min）与就绪后请求连接参数 */
void kr_ble_service(void);

void kr_ble_clear_bonds(void);
void kr_ble_print_status(void);
