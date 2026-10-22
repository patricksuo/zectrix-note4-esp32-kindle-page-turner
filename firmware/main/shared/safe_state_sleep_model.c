/* Step order for handing the GPIO17 power latch to RTC hold before deep sleep (pure logic). Result: RTC_HELD = may sleep; DIG_RECOVERED = cancel; PARK = stop, do not restart or sleep. */
#include "safe_state_sleep_model.h"
static const ss_sleep_op_t RTC_SEQ[] = {
    SS_OP_RTC_SET_DIR, SS_OP_RTC_PULLDOWN_DIS, SS_OP_RTC_PULLUP_DIS,
    SS_OP_RTC_SET_LEVEL, SS_OP_RTC_INIT, SS_OP_HOLD_DIS,
    SS_OP_RTC_READ_PRE, SS_OP_RTC_HOLD_EN, SS_OP_RTC_READ_POST,
};
#define RTC_SEQ_N ((int)(sizeof RTC_SEQ / sizeof RTC_SEQ[0]))
static const ss_sleep_op_t REC_SEQ[] = {
    SS_OP_REC_DIG_CONFIG, SS_OP_REC_DIG_SET, SS_OP_REC_DIG_HOLD_EN,
    SS_OP_REC_DIG_READ,
};
#define REC_SEQ_N ((int)(sizeof REC_SEQ / sizeof REC_SEQ[0]))
void ss_sleep_model_init(ss_sleep_model_t *m)
{
    m->result = SS_VBAT_PENDING;
    m->step = 0;
    m->in_recovery = false;
    m->rtc_failed = false;
    m->n_trace = 0;
    m->overflow = false;
    for (int i = 0; i < SS_SLEEP_TRACE_MAX; i++) {
        m->trace[i] = SS_OP_DONE;
        m->trace_ok[i] = false;
    }
}
bool ss_sleep_model_done(const ss_sleep_model_t *m)
{
    return m->result != SS_VBAT_PENDING;
}
ss_sleep_op_t ss_sleep_model_next(const ss_sleep_model_t *m)
{
    if (ss_sleep_model_done(m)) {
        return SS_OP_DONE;
    }
    const ss_sleep_op_t *seq = m->in_recovery ? REC_SEQ : RTC_SEQ;
    int n = m->in_recovery ? REC_SEQ_N : RTC_SEQ_N;
    if (m->step < 0 || m->step >= n) {
        return SS_OP_DONE;
    }
    return seq[m->step];
}
static void trace_push(ss_sleep_model_t *m, ss_sleep_op_t op, bool ok)
{
    if (m->n_trace >= SS_SLEEP_TRACE_MAX) {
        m->overflow = true;
        return;
    }
    m->trace[m->n_trace] = op;
    m->trace_ok[m->n_trace] = ok;
    m->n_trace++;
}
void ss_sleep_model_feed(ss_sleep_model_t *m, bool ok)
{
    ss_sleep_op_t op = ss_sleep_model_next(m);
    if (op == SS_OP_DONE) {
        return;
    }
    trace_push(m, op, ok);
    if (!m->in_recovery) {
        if (!ok) {
            m->rtc_failed = true;
            m->in_recovery = true;
            m->step = 0;
            return;
        }
        m->step++;
        if (m->step >= RTC_SEQ_N) {
            m->result = SS_VBAT_RTC_HELD;
        }
        return;
    }
    m->step++;
    if (m->step >= REC_SEQ_N) {
        bool read_ok = false;
        for (int i = m->n_trace - 1; i >= 0; i--) {
            if (m->trace[i] == SS_OP_REC_DIG_READ) {
                read_ok = m->trace_ok[i];
                break;
            }
        }
        m->result = read_ok ? SS_VBAT_DIG_RECOVERED : SS_VBAT_PARK;
    }
}
bool ss_sleep_model_may_deep_sleep(const ss_sleep_model_t *m)
{
    return m->result == SS_VBAT_RTC_HELD;
}
ss_vbat_result_t ss_sleep_model_run(ss_sleep_model_t *m,
                                    const bool fail[SS_OP_COUNT])
{
    ss_sleep_model_init(m);
    int guard = 0;
    while (!ss_sleep_model_done(m) && guard++ <= SS_SLEEP_TRACE_MAX) {
        ss_sleep_op_t op = ss_sleep_model_next(m);
        if (op == SS_OP_DONE) {
            break;
        }
        ss_sleep_model_feed(m, !fail[op]);
    }
    return m->result;
}
const char *ss_sleep_op_name(ss_sleep_op_t op)
{
    switch (op) {
    case SS_OP_RTC_SET_DIR: return "rtc_set_direction";
    case SS_OP_RTC_PULLDOWN_DIS: return "rtc_pulldown_dis";
    case SS_OP_RTC_PULLUP_DIS: return "rtc_pullup_dis";
    case SS_OP_RTC_SET_LEVEL: return "rtc_set_level(1)";
    case SS_OP_RTC_INIT: return "rtc_gpio_init";
    case SS_OP_HOLD_DIS: return "rtc_hold_dis";
    case SS_OP_RTC_READ_PRE: return "rtc_read(hold前)";
    case SS_OP_RTC_HOLD_EN: return "rtc_hold_en";
    case SS_OP_RTC_READ_POST: return "rtc_read(hold後)";
    case SS_OP_REC_DIG_CONFIG: return "rec:gpio_config";
    case SS_OP_REC_DIG_SET: return "rec:set_level(1)";
    case SS_OP_REC_DIG_HOLD_EN: return "rec:gpio_hold_en";
    case SS_OP_REC_DIG_READ: return "rec:gpio_read";
    case SS_OP_DONE: return "DONE";
    default: return "?";
    }
}
const char *ss_vbat_result_name(ss_vbat_result_t r)
{
    switch (r) {
    case SS_VBAT_PENDING: return "PENDING";
    case SS_VBAT_RTC_HELD: return "RTC_HELD";
    case SS_VBAT_DIG_RECOVERED: return "DIG_RECOVERED";
    case SS_VBAT_PARK: return "PARK";
    default: return "?";
    }
}
