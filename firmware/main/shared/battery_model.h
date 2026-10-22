/* Battery level model: charge-line debounce, 3-sample median filter, mV -> %, 4 segments with hysteresis. */
#ifndef BATTERY_MODEL_H
#define BATTERY_MODEL_H 
#include <stdbool.h>
#include <stdint.h>
#define BM_STABLE_MS 400
#define BM_RING_N 3
#define BM_HYST_PCT 5
#define BM_SEG_MAX 4
#define BM_RESAMPLE_MS 1000
#define BM_CHG_COMP_MV 150
typedef struct {
    bool valid;
    uint16_t mv;
    uint8_t pct;
    uint8_t seg;
    bool charging;
    bool stdby_raw;
    bool low;
    uint8_t rest_n;
    uint16_t mv_diag;
    uint32_t seq;
    uint32_t chrome_seq;
} battery_snap_t;
typedef struct {
    int chrg_raw, stdby_raw;
    int64_t chrg_since_ms, stdby_since_ms;
    int chrg_stable, stdby_stable;
    bool chrg_ever_known;
    uint16_t ring[BM_RING_N];
    int ring_n;
    bool ring_fresh;
    bool seg_defined;
    bool last_burst_ok;
    bool resample_pending;
    int64_t resample_at_ms;
    bool cali_ok;
    uint8_t pub_seg;
    bool pub_charging;
    bool pub_visible;
    battery_snap_t snap;
} battery_model_t;
extern const uint8_t BM_SEG_THRESH[BM_SEG_MAX];
uint8_t battery_model_pct(uint16_t mv);
uint8_t battery_model_seg_raw(uint8_t pct);
void battery_model_init(battery_model_t *m, bool cali_ok);
bool battery_model_tick_lines(battery_model_t *m, int chrg_l, int stdby_h,
                              int64_t now_ms);
void battery_model_burst_ok(battery_model_t *m, uint16_t mv, int64_t now_ms);
void battery_model_burst_fail(battery_model_t *m, int64_t now_ms);
bool battery_model_sample_due(const battery_model_t *m, bool period_due,
                              int64_t now_ms);
static inline bool battery_model_rest_sample(const battery_snap_t *s,
                                             uint16_t *mv_out, uint8_t *pct_out)
{
    if (!s->valid || s->charging || s->rest_n != BM_RING_N) {
        return false;
    }
    *mv_out = s->mv;
    *pct_out = s->pct;
    return true;
}
bool battery_model_chrg_known(const battery_model_t *m);
#endif
