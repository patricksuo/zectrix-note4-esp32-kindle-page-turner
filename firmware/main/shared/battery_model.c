/* Battery level model: charge-line debounce, 3-sample median filter, mV -> %, 4 segments with hysteresis. */
#include "battery_model.h"
#include <string.h>
const uint8_t BM_SEG_THRESH[BM_SEG_MAX] = { 10, 25, 50, 75 };
uint8_t battery_model_pct(uint16_t mv)
{
    int64_t v = mv;
    int64_t p = (-v * v + 9016 * v - 19189000) / 10000;
    if (p < 0) return 0;
    if (p > 100) return 100;
    return (uint8_t)p;
}
uint8_t battery_model_seg_raw(uint8_t pct)
{
    uint8_t seg = 0;
    for (int i = 0; i < BM_SEG_MAX; i++) {
        if (pct >= BM_SEG_THRESH[i]) seg = (uint8_t)(i + 1);
    }
    return seg;
}
void battery_model_init(battery_model_t *m, bool cali_ok)
{
    memset(m, 0, sizeof *m);
    m->chrg_raw = m->stdby_raw = -1;
    m->chrg_stable = m->stdby_stable = -1;
    m->cali_ok = cali_ok;
    m->ring_fresh = true;
}
static void derive(battery_model_t *m)
{
    battery_snap_t *s = &m->snap;
    bool valid = m->cali_ok && m->last_burst_ok && (s->charging || m->seg_defined);
    bool low = valid && !s->charging && s->seg == 0;
    bool sem_changed = valid != s->valid || low != s->low
                    || s->seg != m->pub_seg || s->charging != m->pub_charging;
    bool vis_changed = valid != m->pub_visible;
    bool dto_changed = valid && (s->charging != m->pub_charging
                                 || s->seg != m->pub_seg);
    s->valid = valid;
    s->low = low;
    if (sem_changed) s->seq++;
    if (vis_changed || dto_changed) s->chrome_seq++;
    m->pub_seg = s->seg;
    m->pub_charging = s->charging;
    m->pub_visible = valid;
}
static bool line_tick(int raw, int *last_raw, int64_t *since, int *stable,
                      int64_t now_ms)
{
    if (raw != *last_raw) {
        *last_raw = raw;
        *since = now_ms;
    }
    if (now_ms - *since >= BM_STABLE_MS && *stable != raw) {
        *stable = raw;
        return true;
    }
    return false;
}
bool battery_model_tick_lines(battery_model_t *m, int chrg_l, int stdby_h,
                              int64_t now_ms)
{
    bool chrg_changed = line_tick(chrg_l ? 1 : 0, &m->chrg_raw, &m->chrg_since_ms,
                                  &m->chrg_stable, now_ms);
    bool stdby_changed = line_tick(stdby_h ? 1 : 0, &m->stdby_raw,
                                   &m->stdby_since_ms, &m->stdby_stable, now_ms);
    if (stdby_changed) {
        m->snap.stdby_raw = (m->stdby_stable == 1);
    }
    if (!chrg_changed) {
        return false;
    }
    bool charging = (m->chrg_stable == 0);
    bool was = m->snap.charging;
    bool first_known = !m->chrg_ever_known;
    m->chrg_ever_known = true;
    m->snap.charging = charging;
    if (was && !charging) {
        m->ring_n = 0;
        m->snap.rest_n = 0;
        m->ring_fresh = true;
        m->resample_pending = true;
        m->resample_at_ms = now_ms + BM_RESAMPLE_MS;
    }
    if (first_known && !m->resample_pending) {
        m->resample_pending = true;
        m->resample_at_ms = now_ms;
    }
    derive(m);
    return true;
}
static uint16_t ring_median(const battery_model_t *m)
{
    if (m->ring_n <= 0) return 0;
    if (m->ring_n == 1) return m->ring[0];
    if (m->ring_n == 2) return (uint16_t)(((uint32_t)m->ring[0] + m->ring[1]) / 2);
    uint16_t a = m->ring[0], b = m->ring[1], c = m->ring[2];
    if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
    if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
    return c;
}
static uint8_t seg_hyst(uint8_t cur, uint8_t pct)
{
    uint8_t raw = battery_model_seg_raw(pct);
    if (raw < cur) return raw;
    uint8_t seg = cur;
    while (seg < BM_SEG_MAX && pct >= BM_SEG_THRESH[seg] + BM_HYST_PCT) {
        seg++;
    }
    return seg;
}
void battery_model_burst_ok(battery_model_t *m, uint16_t mv, int64_t now_ms)
{
    (void)now_ms;
    m->snap.mv_diag = mv;
    m->last_burst_ok = true;
    m->resample_pending = false;
    if (m->snap.charging) {
        uint16_t comp = mv > BM_CHG_COMP_MV ? (uint16_t)(mv - BM_CHG_COMP_MV) : 0;
        uint8_t cseg = battery_model_seg_raw(battery_model_pct(comp));
        if (!m->seg_defined) {
            m->snap.seg = cseg;
            m->seg_defined = true;
        } else if (cseg > m->snap.seg) {
            m->snap.seg = cseg;
        }
    } else {
        if (m->ring_n < BM_RING_N) {
            m->ring[m->ring_n++] = mv;
        } else {
            m->ring[0] = m->ring[1];
            m->ring[1] = m->ring[2];
            m->ring[2] = mv;
        }
        m->snap.rest_n = (uint8_t)m->ring_n;
        m->snap.mv = ring_median(m);
        m->snap.pct = battery_model_pct(m->snap.mv);
        if (m->ring_fresh) {
            m->snap.seg = battery_model_seg_raw(m->snap.pct);
            m->ring_fresh = false;
            m->seg_defined = true;
        } else {
            m->snap.seg = seg_hyst(m->snap.seg, m->snap.pct);
        }
    }
    derive(m);
}
void battery_model_burst_fail(battery_model_t *m, int64_t now_ms)
{
    m->last_burst_ok = false;
    m->resample_pending = true;
    m->resample_at_ms = now_ms + BM_RESAMPLE_MS;
    derive(m);
}
bool battery_model_chrg_known(const battery_model_t *m)
{
    return m->chrg_stable >= 0;
}
bool battery_model_sample_due(const battery_model_t *m, bool period_due,
                              int64_t now_ms)
{
    if (!m->cali_ok || !battery_model_chrg_known(m)) {
        return false;
    }
    if (m->resample_pending && now_ms >= m->resample_at_ms) {
        return true;
    }
    return period_due;
}
