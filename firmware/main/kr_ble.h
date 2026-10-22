/* BLE side: HID-over-GATT peripheral posing as Kindle Click (VID 0x0171 / PID 0x0443).
 * Protocol sources: zuyan9 PROTOCOL.md (reverse-engineered from Kindle 5.20.1) + nes-dpad protocol_test measurements,
 * summarized in ../../docs/PROTOCOL.md. All functions are called from the app task; GAP events run in the NimBLE host task. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KR_REPORT_LEN 10 /* [modifier][reserved][key1..key6][consumer_lo][consumer_hi] */

void kr_ble_start(void);

bool kr_ble_connected(void);
/* Encrypted, and 800 ms of quiet since the Kindle's last CCCD write / link change (or 5 s after encryption as a fallback).
 * Reports sent before this are silently dropped by the Kindle. */
bool kr_ble_ready(void);
bool kr_ble_has_bonds(void);

/* Sends only when subscribed; false = not sent (caller retries next round) */
bool kr_ble_send_report(const uint8_t r[KR_REPORT_LEN]);

void kr_ble_set_battery(uint8_t pct);
/* Call every loop: battery notifications (on connect / after subscribe / every 15 min) and the post-ready connection parameter request */
void kr_ble_service(void);

void kr_ble_clear_bonds(void);
void kr_ble_print_status(void);
