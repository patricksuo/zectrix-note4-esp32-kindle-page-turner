#include "kr_ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

#define DEVICE_NAME "Amazon Kindle Remote" /* optional; shows up as "Kindle Click" in the Kindle's list */
#define FW_REV      "1.0.0"                /* shown under Settings › Accessories */

#define ADV_ITVL             32    /* units of 0.625 ms = 20 ms: the Kindle reconnects via background scanning, shorter is faster */
#define READY_QUIET_MS       800
#define READY_FALLBACK_MS    5000
#define CONN_PARAM_DELAY_MS  1500  /* request only after ready, so we don't compete with the Kindle's discovery phase */
#define BATT_SUBSCRIBED_MS   1000  /* wait this long after a CCCD write before notifying */
#define BATT_RENOTIFY_MS     (15u * 60u * 1000u)

/* Manufacturer data in the advertisement: company 0x0171 (little-endian) + PID 0x0443 (big-endian). Must be the first 0xFF. */
static const uint8_t ADV_MFG[] = { 0x71, 0x01, 0x04, 0x43 };
/* PnP ID: VID source 0x01 (BT SIG), VID 0x0171, PID 0x0443, version 0x0100, all little-endian */
static const uint8_t PNP_ID[] = { 0x01, 0x71, 0x01, 0x43, 0x04, 0x00, 0x01 };
/* bcdHID 1.11, country 0, flags RemoteWake|NormallyConnectable (same as zuyan9) */
static const uint8_t HID_INFO[] = { 0x11, 0x01, 0x00, 0x03 };
/* Report ID 1, Input */
static const uint8_t REPORT_REF[] = { 0x01, 0x01 };

/* Single Application Collection: keyboard and Consumer usages share one evdev node,
 * and that node has EV_REP, so holding a brightness key keeps adjusting (zuyan9 PROTOCOL.md section 4). */
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, /* Generic Desktop / Keyboard / Application */
    0x85, 0x01,                         /*   Report ID 1 */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, /*   modifiers */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01, /*   reserved */
    0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00, /*   6 keys */
    0x05, 0x0C,                         /*   Consumer */
    0x19, 0x00, 0x2A, 0xFF, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00, /*   16-bit consumer usage */
    0xC0,
};

enum {
    A_HID_INFO, A_REPORT_MAP, A_CTRL_POINT, A_REPORT, A_REPORT_REF,
    A_BATTERY, A_PNP, A_FW_REV, A_MANUF,
};

static uint16_t s_report_handle;
static uint16_t s_batt_handle;
static uint8_t  s_own_addr_type;

/* ---- Link state: written by the GAP callback (host task), read by the app task; guarded by a spinlock ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_connected_at, s_secured_at, s_cccd_at, s_disc_done_at, s_ready_at;
static uint32_t s_cccd_writes;
static bool s_secured, s_fast_interval, s_ready, s_params_requested;
static bool s_report_sub, s_batt_sub;
/* Everything below is touched only in the app task */
static bool     s_batt_sent;
static uint32_t s_batt_sent_cccd, s_batt_sent_at;
static volatile uint8_t s_battery = 100;
static bool     s_batt_known; /* don't notify until there has been a real reading (or a serial override) */
static uint8_t  s_report[KR_REPORT_LEN];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void adv_start(void);

/* ======================== GATT ======================== */

static int append(struct os_mbuf *om, const void *d, size_t n)
{
    return os_mbuf_append(om, d, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int attr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    switch ((int)(intptr_t)arg) {
    case A_HID_INFO:   return append(ctxt->om, HID_INFO, sizeof HID_INFO);
    case A_REPORT_MAP: return append(ctxt->om, REPORT_MAP, sizeof REPORT_MAP);
    case A_CTRL_POINT: return 0; /* Suspend / Exit Suspend: accepted, ignored */
    case A_REPORT:     return append(ctxt->om, s_report, sizeof s_report);
    case A_REPORT_REF: return append(ctxt->om, REPORT_REF, sizeof REPORT_REF);
    case A_BATTERY: {
        uint8_t b = s_battery;
        return append(ctxt->om, &b, 1);
    }
    case A_PNP:        return append(ctxt->om, PNP_ID, sizeof PNP_ID);
    case A_FW_REV:     return append(ctxt->om, FW_REV, strlen(FW_REV));
    case A_MANUF:      return append(ctxt->om, "ZECTRIX", 7);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define ARG(x) ((void *)(intptr_t)(x))

/* HID goes first: later BAS/DIS changes then don't move the HID handles the Kindle caches within a session.
 * After any GATT structure change, forget the device on the Kindle + clear bonds here with `c`, then re-pair. */
static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = attr_access, .arg = ARG(A_HID_INFO),
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = attr_access, .arg = ARG(A_REPORT_MAP),
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = attr_access, .arg = ARG(A_CTRL_POINT),
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            {
                /* permissions follow NimBLE-Arduino 1.4's inputReport() (verified working on nes-dpad) */
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = attr_access, .arg = ARG(A_REPORT),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_report_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                      .access_cb = attr_access, .arg = ARG(A_REPORT_REF) },
                    { 0 },
                },
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = attr_access, .arg = ARG(A_BATTERY),
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_batt_handle },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = attr_access, .arg = ARG(A_PNP),
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A26), .access_cb = attr_access, .arg = ARG(A_FW_REV),
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = attr_access, .arg = ARG(A_MANUF),
              .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

/* ======================== GAP ======================== */

static void reset_link_state(uint16_t conn)
{
    taskENTER_CRITICAL(&s_mux);
    s_conn = conn;
    s_connected_at = now_ms();
    s_secured = false;
    s_ready = false;
    s_params_requested = false;
    s_cccd_at = 0;
    s_cccd_writes = 0;
    s_disc_done_at = 0;
    s_fast_interval = false;
    s_report_sub = false;
    s_batt_sub = false;
    taskEXIT_CRITICAL(&s_mux);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    struct ble_gap_conn_desc d;
    (void)arg;

    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            printf("[ble] connect failed status=%d, advertising again\n", ev->connect.status);
            adv_start();
            return 0;
        }
        reset_link_state(ev->connect.conn_handle);
        if (ble_gap_conn_find(ev->connect.conn_handle, &d) == 0) {
            taskENTER_CRITICAL(&s_mux);
            s_fast_interval = (d.conn_itvl == 6);
            taskEXIT_CRITICAL(&s_mux);
            printf("[ble] connected peer=%02X:%02X:%02X:%02X:%02X:%02X (type %d) itvl=%.2f ms lat=%u to=%u ms\n",
                   d.peer_ota_addr.val[5], d.peer_ota_addr.val[4], d.peer_ota_addr.val[3],
                   d.peer_ota_addr.val[2], d.peer_ota_addr.val[1], d.peer_ota_addr.val[0],
                   d.peer_ota_addr.type, d.conn_itvl * 1.25f, d.conn_latency,
                   d.supervision_timeout * 10);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        reset_link_state(BLE_HS_CONN_HANDLE_NONE);
        printf("[ble] disconnected reason=0x%03X, advertising again\n", ev->disconnect.reason);
        adv_start();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0) {
            /* 7.5 ms = the Kindle is doing GATT discovery; leaving it, or dropping to 20 ms with no latency,
             * both signal that the Kindle has finished setting up the remote (zuyan9 PROTOCOL.md section 5) */
            bool fast = (d.conn_itvl == 6);
            taskENTER_CRITICAL(&s_mux);
            if ((s_fast_interval && !fast) || (d.conn_itvl == 16 && d.conn_latency == 0)) {
                s_disc_done_at = now_ms() | 1;
            }
            s_fast_interval = fast;
            uint32_t t0 = s_connected_at;
            taskEXIT_CRITICAL(&s_mux);
            printf("[ble] +%lu ms params itvl=%.2f ms lat=%u to=%u ms (status %d)\n",
                   (unsigned long)(now_ms() - t0), d.conn_itvl * 1.25f, d.conn_latency,
                   d.supervision_timeout * 10, ev->conn_update.status);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            taskENTER_CRITICAL(&s_mux);
            s_secured = d.sec_state.encrypted;
            s_secured_at = now_ms();
            uint32_t t0 = s_connected_at;
            taskEXIT_CRITICAL(&s_mux);
            printf("[sec] +%lu ms encrypted=%d bonded=%d authenticated=%d key_size=%u\n",
                   (unsigned long)(now_ms() - t0), d.sec_state.encrypted, d.sec_state.bonded,
                   d.sec_state.authenticated, d.sec_state.key_size);
        } else {
            printf("[sec] encryption change FAILED status=%d\n", ev->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        const char *what = ev->subscribe.attr_handle == s_report_handle ? "input report"
                         : ev->subscribe.attr_handle == s_batt_handle   ? "battery level"
                                                                        : "other";
        taskENTER_CRITICAL(&s_mux);
        if (ev->subscribe.attr_handle == s_report_handle) { s_report_sub = ev->subscribe.cur_notify; }
        if (ev->subscribe.attr_handle == s_batt_handle) { s_batt_sub = ev->subscribe.cur_notify; }
        /* Only a real CCCD write from the Kindle counts as "Kindle is setting up the remote"; RESTORE is an old subscription restored from the bond */
        if (ev->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_WRITE && ev->subscribe.cur_notify) {
            s_cccd_at = now_ms() | 1;
            s_cccd_writes++;
        }
        uint32_t t0 = s_connected_at;
        taskEXIT_CRITICAL(&s_mux);
        printf("[gatt] +%lu ms %s notify=%d (reason %d)\n", (unsigned long)(now_ms() - t0), what,
               ev->subscribe.cur_notify, ev->subscribe.reason);
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Re-pairing after the Kindle forgot us: drop the old bond, accept the new one */
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
            ble_store_util_delete_peer(&d.peer_id_addr);
        }
        printf("[sec] repeat pairing: old bond dropped\n");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        printf("[sec] unexpected passkey action %d (IO cap should be NoInputNoOutput)\n",
               ev->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_MTU:
        printf("[ble] mtu=%u\n", ev->mtu.value);
        return 0;
    }
    return 0;
}

static void adv_start(void)
{
    if (ble_gap_adv_active()) {
        return;
    }
    /* ADV_IND: Flags 0x06 (BR/EDR not supported; without it the Kindle treats us as dual-mode and uses classic Bluetooth)
     * + manufacturer data + HID UUID. No 0xFE03 (it makes the Kindle skip the Amazon remote branch). */
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.mfg_data = ADV_MFG;
    f.mfg_data_len = sizeof ADV_MFG;
    f.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(0x1812) };
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        printf("[ble] adv_set_fields rc=%d\n", rc);
        return;
    }

    struct ble_hs_adv_fields r = { 0 };
    r.name = (const uint8_t *)DEVICE_NAME;
    r.name_len = strlen(DEVICE_NAME);
    r.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc != 0) {
        printf("[ble] adv_rsp_set_fields rc=%d\n", rc);
        return;
    }

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = ADV_ITVL;
    p.itvl_max = ADV_ITVL;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        printf("[ble] adv_start rc=%d\n", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uint8_t a[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, a, NULL);
    printf("[ble] addr=%02X:%02X:%02X:%02X:%02X:%02X (type %u), advertising as \"%s\" every 20 ms\n",
           a[5], a[4], a[3], a[2], a[1], a[0], s_own_addr_type, DEVICE_NAME);
    adv_start();
}

static void on_reset(int reason)
{
    printf("[ble] host reset reason=%d\n", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void kr_ble_start(void)
{
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        printf("[ble] nimble_port_init failed: %d\n", e);
        return;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Just Works + bonding + Secure Connections. Declaring keyboard/display capability makes the Kindle ask for a PIN.
     * Don't distribute our own IRK (same as zuyan9: always advertise with a fixed address); do take the Kindle's IRK,
     * or the bond can't be found when it reconnects with a resolvable private address. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_svcs);
    }
    if (rc != 0) {
        printf("[ble] GATT table rejected rc=%d\n", rc);
        return;
    }
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

/* ======================== App task side ======================== */

bool kr_ble_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

bool kr_ble_has_bonds(void)
{
    int n = 0;
    return ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) == 0 && n > 0;
}

/* Ported from linkReady() in zuyan9's OpenKindleClick.ino */
bool kr_ble_ready(void)
{
    uint32_t now = now_ms();
    bool became = false, quiet = false;
    uint32_t t0 = 0, sec = 0, c = 0, dd = 0, writes = 0;

    taskENTER_CRITICAL(&s_mux);
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_secured) {
        taskEXIT_CRITICAL(&s_mux);
        return false;
    }
    if (!s_ready) {
        c = s_cccd_at;
        dd = s_disc_done_at;
        uint32_t last = !c ? dd : !dd ? c : (int32_t)(c - dd) > 0 ? c : dd;
        quiet = last && !s_fast_interval && (int32_t)(now - last) >= (int32_t)READY_QUIET_MS;
        if (quiet || now - s_secured_at >= READY_FALLBACK_MS) {
            s_ready = true;
            s_ready_at = now;
            became = true;
            t0 = s_connected_at;
            sec = s_secured_at;
            writes = s_cccd_writes;
        }
    }
    bool r = s_ready;
    taskEXIT_CRITICAL(&s_mux);

    if (became) {
        printf("[link] +%lu ms READY (%s): encrypted +%lu, link change %s%lu, %lu CCCD writes%s%lu\n",
               (unsigned long)(now - t0), quiet ? "host settled" : "fallback",
               (unsigned long)(sec - t0), dd ? "+" : "none ", (unsigned long)(dd ? dd - t0 : 0),
               (unsigned long)writes, c ? ", last +" : "", (unsigned long)(c ? c - t0 : 0));
    }
    return r;
}

static bool notify(uint16_t handle, const void *data, size_t len)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;
    }
    return ble_gatts_notify_custom(conn, handle, om) == 0; /* om is already freed on failure */
}

bool kr_ble_send_report(const uint8_t r[KR_REPORT_LEN])
{
    if (!s_report_sub) {
        return false;
    }
    memcpy(s_report, r, KR_REPORT_LEN);
    return notify(s_report_handle, s_report, KR_REPORT_LEN);
}

static void notify_battery(const char *why)
{
    uint8_t b = s_battery;
    if (!s_batt_sub || !notify(s_batt_handle, &b, 1)) {
        return;
    }
    s_batt_sent = true;
    s_batt_sent_cccd = s_cccd_at;
    s_batt_sent_at = now_ms();
    printf("[bat] %u%% notified (%s)\n", b, why);
}

void kr_ble_set_battery(uint8_t pct)
{
    s_battery = pct;
    s_batt_known = true;
    printf("[bat] level -> %u%%\n", pct);
    if (kr_ble_ready()) {
        notify_battery("changed");
    }
}

void kr_ble_service(void)
{
    if (!kr_ble_ready()) {
        s_batt_sent = false;
        return;
    }
    uint32_t now = now_ms();

    /* The Kindle only accepts battery notifications, and only after HID is open: send after ready, again after a CCCD write, and periodically */
    uint32_t c = s_cccd_at;
    const char *why = NULL;
    if (!s_batt_known) { why = NULL; }
    else if (!s_batt_sent) { why = "connected"; }
    else if (c != s_batt_sent_cccd && (int32_t)(now - c) >= BATT_SUBSCRIBED_MS) { why = "subscribed"; }
    else if (now - s_batt_sent_at >= BATT_RENOTIFY_MS) { why = "periodic"; }
    if (why) {
        notify_battery(why);
    }

    /* The Kindle pins the interval at 20 ms but accepts the latency / timeout the peripheral requests (verified at handoff: 30 / 6 s) */
    if (!s_params_requested && now - s_ready_at >= CONN_PARAM_DELAY_MS) {
        s_params_requested = true;
        struct ble_gap_upd_params p = {
            .itvl_min = 16, .itvl_max = 16, .latency = 30, .supervision_timeout = 600,
        };
        int rc = ble_gap_update_params(s_conn, &p);
        printf("[ble] requested itvl 20 ms lat 30 to 6000 ms rc=%d\n", rc);
    }
}

void kr_ble_clear_bonds(void)
{
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    int rc = ble_store_clear();
    printf("[sec] all bonds cleared rc=%d (also Forget the remote on the Kindle)\n", rc);
}

void kr_ble_print_status(void)
{
    int bonds = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bonds);
    printf("[status] connected=%d encrypted=%d ready=%d report_sub=%d batt_sub=%d bonds=%d battery=%u%% adv=%d\n",
           kr_ble_connected(), s_secured, s_ready, s_report_sub, s_batt_sub, bonds, s_battery,
           ble_gap_adv_active());
    struct ble_gap_conn_desc d;
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn, &d) == 0) {
        printf("[status] itvl=%.2f ms lat=%u to=%u ms bonded=%d\n", d.conn_itvl * 1.25f,
               d.conn_latency, d.supervision_timeout * 10, d.sec_state.bonded);
    }
}
