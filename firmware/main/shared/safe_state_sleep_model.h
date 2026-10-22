/* Step order for handing the GPIO17 power latch to RTC hold before deep sleep (pure logic). Result: RTC_HELD = may sleep; DIG_RECOVERED = cancel; PARK = stop, do not restart or sleep. */
#ifndef SAFE_STATE_SLEEP_MODEL_H
#define SAFE_STATE_SLEEP_MODEL_H 
#include <stdbool.h>
typedef enum {
    SS_OP_RTC_SET_DIR = 0,
    SS_OP_RTC_PULLDOWN_DIS,
    SS_OP_RTC_PULLUP_DIS,
    SS_OP_RTC_SET_LEVEL,
    SS_OP_RTC_INIT,
    SS_OP_HOLD_DIS,
    SS_OP_RTC_READ_PRE,
    SS_OP_RTC_HOLD_EN,
    SS_OP_RTC_READ_POST,
    SS_OP_REC_DIG_CONFIG,
    SS_OP_REC_DIG_SET,
    SS_OP_REC_DIG_HOLD_EN,
    SS_OP_REC_DIG_READ,
    SS_OP_COUNT,
    SS_OP_DONE = SS_OP_COUNT,
} ss_sleep_op_t;
typedef enum {
    SS_VBAT_PENDING = 0,
    SS_VBAT_RTC_HELD,
    SS_VBAT_DIG_RECOVERED,
    SS_VBAT_PARK,
} ss_vbat_result_t;
#define SS_SLEEP_TRACE_MAX 32
typedef struct {
    ss_vbat_result_t result;
    int step;
    bool in_recovery;
    bool rtc_failed;
    ss_sleep_op_t trace[SS_SLEEP_TRACE_MAX];
    bool trace_ok[SS_SLEEP_TRACE_MAX];
    int n_trace;
    bool overflow;
} ss_sleep_model_t;
void ss_sleep_model_init(ss_sleep_model_t *m);
ss_sleep_op_t ss_sleep_model_next(const ss_sleep_model_t *m);
void ss_sleep_model_feed(ss_sleep_model_t *m, bool ok);
bool ss_sleep_model_done(const ss_sleep_model_t *m);
bool ss_sleep_model_may_deep_sleep(const ss_sleep_model_t *m);
const char *ss_sleep_op_name(ss_sleep_op_t op);
const char *ss_vbat_result_name(ss_vbat_result_t r);
ss_vbat_result_t ss_sleep_model_run(ss_sleep_model_t *m,
                                    const bool fail[SS_OP_COUNT]);
#endif
